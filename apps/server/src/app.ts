import { randomBytes, scrypt as scryptCb, timingSafeEqual } from 'node:crypto';
import { promisify } from 'node:util';
import Fastify, { type FastifyReply, type FastifyRequest } from 'fastify';
import cors from '@fastify/cors';
import jwt from '@fastify/jwt';
import rateLimit from '@fastify/rate-limit';
import { z } from 'zod';
import { authorize, listenedMsByPeriod, localDay, OFFLINE_LICENSE_MS, parseSearch, summarize, topArtists, topGenres, topTracks, usageCsv, usageReport, type Plan, type Track } from '@sonora/core';
import type { CatalogService } from './providers/catalog';
import type { LyricsService } from './lyrics';
import type { RecommendationService } from './recs/service';
import { SpotifyImportError, summarizeImport, type SpotifySyncService } from './spotify/sync';
import type { Repos } from './repos/types';

const scrypt = promisify(scryptCb) as (pw: string, salt: Buffer, len: number) => Promise<Buffer>;

async function hashPassword(pw: string): Promise<string> {
  const salt = randomBytes(16);
  return `${salt.toString('hex')}:${(await scrypt(pw, salt, 64)).toString('hex')}`;
}

async function verifyPassword(pw: string, stored: string): Promise<boolean> {
  const [salt, hash] = stored.split(':');
  const expected = Buffer.from(hash!, 'hex');
  const got = await scrypt(pw, Buffer.from(salt!, 'hex'), expected.length);
  return timingSafeEqual(expected, got);
}

declare module '@fastify/jwt' {
  interface FastifyJWT {
    payload: { sub: string };
    user: { sub: string };
  }
}

export interface Deps {
  repos: Repos;
  catalog: CatalogService;
  lyrics: LyricsService;
  recs: RecommendationService;
  /** Ausente quando SPOTIFY_CLIENT_ID não está configurado. */
  spotify?: SpotifySyncService;
  jwtSecret: string;
  adminToken?: string;
  trialDays?: number;
  logger?: boolean;
}

const playEventSchema = z.object({
  id: z.string().min(8).max(64),
  trackId: z.string(),
  artistIds: z.array(z.string()),
  genres: z.array(z.string()),
  startedAt: z.number().int(),
  listenedMs: z.number().int().min(0).max(24 * 3_600_000),
  trackDurationMs: z.number().int().min(0),
  skipped: z.boolean(),
  source: z.object({ type: z.enum(['album', 'playlist', 'artist', 'radio', 'search', 'library', 'queue']) }).passthrough(),
  deviceId: z.string(),
  tzOffsetMin: z.number().int().min(-840).max(840),
});

const opSchema = z.object({
  entity: z.enum(['playlist', 'playlistEntry', 'like', 'savedAlbum', 'followArtist', 'followPlaylist', 'settings', 'playback']),
  entityId: z.string().max(200),
  field: z.string().max(64),
  value: z.unknown(),
  hlc: z.string().max(120),
  deviceId: z.string().max(64),
});

const rangeSchema = z.object({
  from: z.coerce.number().int().optional(),
  to: z.coerce.number().int().optional(),
  tz: z.coerce.number().int().default(0),
});

export async function buildApp(deps: Deps) {
  const { repos, catalog, lyrics, recs } = deps;
  const app = Fastify({ logger: deps.logger ?? false, bodyLimit: 2 * 1024 * 1024 });

  await app.register(cors, { origin: true });
  await app.register(jwt, { secret: deps.jwtSecret, sign: { expiresIn: '30d' } });
  await app.register(rateLimit, { max: 600, timeWindow: '1 minute' });

  app.setErrorHandler((err, _req, reply) => {
    if (err instanceof z.ZodError) return reply.code(400).send({ error: 'validation', issues: err.issues });
    const status = (err as { statusCode?: number }).statusCode ?? 500;
    if (status >= 500) app.log.error(err);
    return reply.code(status).send({ error: (err as Error).message });
  });

  const auth = async (req: FastifyRequest, reply: FastifyReply) => {
    try {
      await req.jwtVerify();
    } catch {
      return reply.code(401).send({ error: 'unauthorized' });
    }
  };
  const uid = (req: FastifyRequest) => req.user.sub;

  const admin = async (req: FastifyRequest, reply: FastifyReply) => {
    const token = req.headers['x-admin-token'];
    if (!deps.adminToken || typeof token !== 'string' || token.length !== deps.adminToken.length || !timingSafeEqual(Buffer.from(token), Buffer.from(deps.adminToken))) {
      return reply.code(404).send({ error: 'not found' });
    }
  };

  const entitlementOf = async (userId: string) => (await repos.users.byId(userId))?.entitlement ?? null;

  app.get('/health', async () => ({ ok: true, providers: catalog.providers.map((p) => p.id) }));

  // ---------- Auth ----------
  const credentials = z.object({
    email: z.string().email(),
    password: z.string().min(8),
    displayName: z.string().min(1).max(60).optional(),
    country: z.string().length(2).optional(),
  });

  app.post('/auth/register', async (req, reply) => {
    const body = credentials.parse(req.body);
    if (await repos.users.byEmail(body.email.toLowerCase())) return reply.code(409).send({ error: 'email já cadastrado' });
    // País da conta: informado no cadastro ou detectado pela CDN (ex.: CF-IPCountry); define o território licenciado.
    const detected = req.headers['cf-ipcountry'] ?? req.headers['x-country'];
    const country = (body.country ?? (typeof detected === 'string' ? detected : 'BR')).toUpperCase();
    const trialDays = deps.trialDays ?? 0;
    const user = await repos.users.create({
      email: body.email.toLowerCase(),
      displayName: body.displayName ?? body.email.split('@')[0]!,
      passwordHash: await hashPassword(body.password),
      entitlement: trialDays > 0 ? { plan: 'trial', validUntil: Date.now() + trialDays * 86_400_000, country } : { plan: 'none', validUntil: 0, country },
    });
    return { token: app.jwt.sign({ sub: user.id }), user: { id: user.id, displayName: user.displayName } };
  });

  app.post('/auth/login', async (req, reply) => {
    const body = credentials.parse(req.body);
    const user = await repos.users.byEmail(body.email.toLowerCase());
    if (!user || !(await verifyPassword(body.password, user.passwordHash))) return reply.code(401).send({ error: 'credenciais inválidas' });
    return { token: app.jwt.sign({ sub: user.id }), user: { id: user.id, displayName: user.displayName } };
  });

  app.get('/me', { preHandler: auth }, async (req) => {
    const u = await repos.users.byId(uid(req));
    return u && { id: u.id, email: u.email, displayName: u.displayName, entitlement: u.entitlement };
  });

  // ---------- Catálogo e busca ----------
  app.get('/search', { preHandler: auth }, async (req) => {
    const { q, limit } = z.object({ q: z.string().max(500), limit: z.coerce.number().int().min(1).max(100).default(30) }).parse(req.query);
    const query = parseSearch(q);
    if (query.lyrics) {
      // Busca por letra: resolve pares artista/título e procura cada um no catálogo.
      const hits = await lyrics.searchByLyrics(query.lyrics);
      const resolved = await Promise.all(hits.slice(0, 8).map((h) => catalog.search({ ...query, lyrics: undefined, text: h.title, artist: h.artist }, 1)));
      return { query, tracks: resolved.flatMap((r) => r.tracks), albums: [], artists: [], errors: resolved.flatMap((r) => r.errors) };
    }
    return { query, ...(await catalog.search(query, limit)) };
  });

  app.get('/tracks/:id', { preHandler: auth }, async (req, reply) => {
    const t = await catalog.track((req.params as { id: string }).id);
    return t ?? reply.code(404).send({ error: 'not found' });
  });

  app.post('/tracks/batch', { preHandler: auth }, async (req) => {
    const { ids } = z.object({ ids: z.array(z.string()).max(200) }).parse(req.body);
    return { tracks: await catalog.tracks(ids) };
  });

  app.get('/albums/:id', { preHandler: auth }, async (req, reply) => {
    const a = await catalog.album((req.params as { id: string }).id);
    return a ?? reply.code(404).send({ error: 'not found' });
  });

  app.get('/artists/:id', { preHandler: auth }, async (req, reply) => {
    const a = await catalog.artist((req.params as { id: string }).id);
    return a ?? reply.code(404).send({ error: 'not found' });
  });

  // ---------- Autorização de reprodução (exigência dos licenciantes) ----------
  app.get('/tracks/:id/stream', { preHandler: auth }, async (req, reply) => {
    const { id } = req.params as { id: string };
    const { deviceId, purpose } = z.object({ deviceId: z.string().max(64), purpose: z.enum(['stream', 'download']).default('stream') }).parse(req.query);
    const t = await catalog.track(id);
    if (!t) return reply.code(404).send({ error: 'not found' });
    const e = await entitlementOf(uid(req));
    const decision = authorize(t, e);
    if (!decision.ok) return reply.code(403).send({ error: decision.reason });
    const sources = await catalog.streamSources(t, { userId: uid(req), deviceId, country: e?.country ?? 'BR', purpose });
    return { sources, ...(purpose === 'download' ? { licenseExpiresAt: Date.now() + OFFLINE_LICENSE_MS } : {}) };
  });

  /** Revalida licenças offline (o app chama ao sincronizar; prazo de 30 dias sem rede). */
  app.post('/downloads/renew', { preHandler: auth }, async (req) => {
    const { ids } = z.object({ ids: z.array(z.string()).max(5000) }).parse(req.body);
    const e = await entitlementOf(uid(req));
    const expiresAt = Date.now() + OFFLINE_LICENSE_MS;
    const renewed: string[] = [];
    const revoked: string[] = [];
    for (let i = 0; i < ids.length; i += 200) {
      const chunk = ids.slice(i, i + 200);
      const found = new Map((await catalog.tracks(chunk)).map((t) => [t.id, t]));
      for (const id of chunk) {
        const t = found.get(id);
        // Faixa sumida do catálogo = licença retirada pelo detentor.
        if (t && authorize(t, e).ok) renewed.push(id);
        else revoked.push(id);
      }
    }
    return { renewed, revoked, expiresAt };
  });

  app.get('/tracks/:id/lyrics', { preHandler: auth }, async (req, reply) => {
    const t = await catalog.track((req.params as { id: string }).id);
    if (!t) return reply.code(404).send({ error: 'not found' });
    return lyrics.forTrack(t);
  });

  // ---------- Sincronização ----------
  const canRead = async (userId: string, scope: string) => {
    if (scope === `user:${userId}`) return true;
    if (scope.startsWith('playlist:')) return (await repos.playlists.role(scope.slice(9), userId)) !== null;
    return false;
  };
  const canWrite = async (userId: string, scope: string) => {
    if (scope === `user:${userId}`) return true;
    if (scope.startsWith('playlist:')) {
      const role = await repos.playlists.role(scope.slice(9), userId);
      return role === 'owner' || role === 'editor';
    }
    return false;
  };

  app.get('/sync/scopes', { preHandler: auth }, async (req) => {
    const shared = await repos.playlists.playlistsOf(uid(req));
    return { scopes: [`user:${uid(req)}`, ...shared.map((p) => `playlist:${p.playlistId}`)], playlists: shared };
  });

  app.post('/sync/push', { preHandler: auth }, async (req, reply) => {
    const { scope, ops } = z.object({ scope: z.string(), ops: z.array(opSchema).max(1000) }).parse(req.body);
    if (!(await canWrite(uid(req), scope))) return reply.code(403).send({ error: 'forbidden' });
    return { cursor: await repos.sync.append(scope, uid(req), ops as never) };
  });

  app.get('/sync/pull', { preHandler: auth }, async (req, reply) => {
    const { scope, cursor, limit } = z.object({ scope: z.string(), cursor: z.coerce.number().int().min(0).default(0), limit: z.coerce.number().int().min(1).max(5000).default(1000) }).parse(req.query);
    if (!(await canRead(uid(req), scope))) return reply.code(403).send({ error: 'forbidden' });
    const ops = await repos.sync.since(scope, cursor, limit);
    return { ops, cursor: ops.length ? ops[ops.length - 1]!.seq : cursor, hasMore: ops.length === limit };
  });

  // ---------- Playlists colaborativas ----------
  app.post('/playlists/:id/claim', { preHandler: auth }, async (req, reply) => {
    const { id } = req.params as { id: string };
    const existing = await repos.playlists.role(id, uid(req));
    if (existing) return { role: existing };
    // Primeira pessoa a registrar o ID vira dona; IDs são UUIDs gerados no cliente.
    if (await repos.playlists.hasMembers(id)) return reply.code(409).send({ error: 'playlist já existe' });
    await repos.playlists.setMember(id, uid(req), 'owner');
    return { role: 'owner' };
  });

  app.post('/playlists/:id/invites', { preHandler: auth }, async (req, reply) => {
    const { id } = req.params as { id: string };
    if ((await repos.playlists.role(id, uid(req))) !== 'owner') return reply.code(403).send({ error: 'somente o dono convida' });
    const token = randomBytes(18).toString('base64url');
    await repos.playlists.createInvite(token, id, uid(req), new Date(Date.now() + 7 * 86_400_000));
    return { token, url: `sonora://invite/${token}` };
  });

  app.post('/invites/:token/accept', { preHandler: auth }, async (req, reply) => {
    const inv = await repos.playlists.consumeInvite((req.params as { token: string }).token);
    if (!inv) return reply.code(404).send({ error: 'convite inválido ou expirado' });
    const current = await repos.playlists.role(inv.playlistId, uid(req));
    if (current !== 'owner') await repos.playlists.setMember(inv.playlistId, uid(req), 'editor');
    return { playlistId: inv.playlistId, scope: `playlist:${inv.playlistId}` };
  });

  app.post('/playlists/:id/follow', { preHandler: auth }, async (req) => {
    const { id } = req.params as { id: string };
    if (!(await repos.playlists.role(id, uid(req)))) await repos.playlists.setMember(id, uid(req), 'follower');
    return { ok: true };
  });

  // ---------- Histórico e estatísticas ----------
  app.post('/plays', { preHandler: auth }, async (req) => {
    const { events } = z.object({ events: z.array(playEventSchema).max(2000) }).parse(req.body);
    const e = await entitlementOf(uid(req));
    // O plano vigente na ingestão é o que vai para o relatório; reprodução de catálogo licenciado sem plano já é barrada em /stream.
    const plan: Plan = e && e.validUntil >= Date.now() ? e.plan : 'none';
    return { accepted: await repos.plays.insert(uid(req), events as never, { country: e?.country ?? 'BR', plan }) };
  });

  app.get('/history', { preHandler: auth }, async (req) => {
    const { before, limit } = z.object({ before: z.coerce.number().int().optional(), limit: z.coerce.number().int().min(1).max(500).default(100) }).parse(req.query);
    const events = await repos.plays.list(uid(req), 0, before ?? Date.now() + 1);
    return { events: events.slice(-limit).reverse() };
  });

  app.get('/stats/summary', { preHandler: auth }, async (req) => {
    const { from, to, tz } = rangeSchema.parse(req.query);
    const now = Date.now();
    const history = await repos.plays.list(uid(req));
    const s = summarize(history, from ?? now - 30 * 86_400_000, to ?? now + 1, localDay(now, tz));
    const tracks = await catalog.tracks(s.topTracks.map((r) => r.id));
    return { ...s, tracks: Object.fromEntries(tracks.map((t) => [t.id, t])), artistNames: await artistNames(s.topArtists.map((a) => a.id), tracks) };
  });

  app.get('/stats/series', { preHandler: auth }, async (req) => {
    const { from, to, period } = rangeSchema.extend({ period: z.enum(['day', 'week', 'month', 'year']).default('day') }).parse(req.query);
    const events = await repos.plays.list(uid(req), from, to);
    const series = [...listenedMsByPeriod(events, period).entries()].sort(([a], [b]) => (a < b ? -1 : 1)).map(([key, ms]) => ({ key, minutes: Math.round(ms / 60_000) }));
    return { period, series };
  });

  app.get('/stats/top', { preHandler: auth }, async (req) => {
    const { from, to, kind, limit } = rangeSchema.extend({ kind: z.enum(['tracks', 'artists', 'genres']), limit: z.coerce.number().int().min(1).max(100).default(50) }).parse(req.query);
    const events = await repos.plays.list(uid(req), from, to);
    const fn = { tracks: topTracks, artists: topArtists, genres: topGenres }[kind];
    return { kind, items: fn(events, limit) };
  });

  async function artistNames(ids: string[], known: Track[]): Promise<Record<string, string>> {
    const names: Record<string, string> = {};
    for (const t of known) for (const a of t.artists) names[a.id] = a.name;
    await Promise.all(
      ids.filter((id) => !names[id]).map(async (id) => {
        const page = await catalog.artist(id).catch(() => null);
        if (page) names[id] = page.artist.name;
      }),
    );
    return names;
  }

  // ---------- Recomendações ----------
  app.get('/recs/home', { preHandler: auth }, async (req) => {
    const [weekly, mixes] = await Promise.all([recs.discoverWeekly(uid(req)), recs.dailyMixes(uid(req))]);
    const recent = (await repos.plays.list(uid(req), Date.now() - 7 * 86_400_000)).slice(-50).reverse();
    const recentTracks = await catalog.tracks([...new Set(recent.map((e) => e.trackId))].slice(0, 12));
    return {
      sections: [
        { id: 'recent', title: 'Tocadas recentemente', tracks: recentTracks },
        { id: 'mixes', title: 'Seus mixes', mixes: [weekly, ...mixes] },
      ],
    };
  });

  app.get('/recs/discover-weekly', { preHandler: auth }, async (req) => recs.discoverWeekly(uid(req)));
  app.get('/recs/daily-mixes', { preHandler: auth }, async (req) => ({ mixes: await recs.dailyMixes(uid(req)) }));

  app.get('/recs/radio', { preHandler: auth }, async (req) => {
    const { seed, exclude } = z.object({ seed: z.string(), exclude: z.string().optional() }).parse(req.query);
    return { tracks: await recs.radio(uid(req), seed, exclude ? exclude.split(',') : []) };
  });

  app.post('/recs/playlist', { preHandler: auth }, async (req) => {
    const { trackIds } = z.object({ trackIds: z.array(z.string()).max(500) }).parse(req.body);
    return { tracks: await recs.forPlaylist(uid(req), trackIds) };
  });

  // ---------- Importação de playlists do Spotify ----------
  const spotify = deps.spotify;
  const spotifyError = (reply: FastifyReply, e: unknown) => {
    if (!(e instanceof SpotifyImportError)) throw e;
    const status = { 'invalid-link': 400, 'not-connected': 409, 'not-allowed': 403, 'not-found': 404, 'spotify-error': 502 }[e.code];
    return reply.code(status).send({ error: e.code, message: e.message });
  };

  app.get('/spotify/status', { preHandler: auth }, async (req) => {
    const link = spotify ? await repos.spotify.getLink(uid(req)) : null;
    return { enabled: !!spotify, clientId: spotify?.client.clientId ?? null, connected: !!link, account: link ? { id: link.spotifyUserId, displayName: link.displayName } : null };
  });

  app.post('/spotify/connect', { preHandler: auth }, async (req, reply) => {
    if (!spotify) return reply.code(404).send({ error: 'spotify-disabled' });
    const { code, codeVerifier, redirectUri } = z.object({ code: z.string().min(1), codeVerifier: z.string().min(43).max(128), redirectUri: z.string().url() }).parse(req.body);
    try {
      return { account: await spotify.connect(uid(req), code, codeVerifier, redirectUri) };
    } catch (e) {
      return reply.code(400).send({ error: 'spotify-auth', message: (e as Error).message });
    }
  });

  app.delete('/spotify/connect', { preHandler: auth }, async (req) => {
    await repos.spotify.deleteLink(uid(req));
    return { ok: true };
  });

  app.get('/spotify/playlists', { preHandler: auth }, async (req) => ({ playlists: (await repos.spotify.listImports(uid(req))).map(summarizeImport) }));

  app.post('/spotify/playlists', { preHandler: auth }, async (req, reply) => {
    if (!spotify) return reply.code(404).send({ error: 'spotify-disabled' });
    const { url } = z.object({ url: z.string().min(1).max(500) }).parse(req.body);
    try {
      return { playlist: await spotify.importPlaylist(uid(req), url) };
    } catch (e) {
      return spotifyError(reply, e);
    }
  });

  /** O app chama ao abrir e ao puxar para atualizar; o servidor também varre sozinho a cada poucos minutos. */
  app.post('/spotify/sync', { preHandler: auth }, async (req, reply) => {
    if (!spotify) return reply.code(404).send({ error: 'spotify-disabled' });
    return { playlists: await spotify.syncUser(uid(req)) };
  });

  app.post('/spotify/playlists/:spotifyId/sync', { preHandler: auth }, async (req, reply) => {
    if (!spotify) return reply.code(404).send({ error: 'spotify-disabled' });
    const p = await repos.spotify.getImport(uid(req), (req.params as { spotifyId: string }).spotifyId);
    if (!p) return reply.code(404).send({ error: 'not-found' });
    return { playlist: summarizeImport(await spotify.syncOne(p, true)) };
  });

  app.delete('/spotify/playlists/:spotifyId', { preHandler: auth }, async (req, reply) => {
    if (!spotify) return reply.code(404).send({ error: 'spotify-disabled' });
    await spotify.stopSyncing(uid(req), (req.params as { spotifyId: string }).spotifyId);
    return { ok: true };
  });

  // ---------- Administração e relatórios para licenciantes ----------
  app.post('/admin/users/:id/entitlement', { preHandler: admin }, async (req, reply) => {
    const body = z.object({ plan: z.enum(['none', 'trial', 'premium', 'family', 'student']), validUntil: z.number().int(), country: z.string().length(2) }).parse(req.body);
    const id = (req.params as { id: string }).id;
    if (!(await repos.users.byId(id))) return reply.code(404).send({ error: 'not found' });
    await repos.users.setEntitlement(id, { ...body, country: body.country.toUpperCase() });
    return { ok: true };
  });

  /**
   * Relatório mensal de uso por ISRC × território × plano, com rateio do pool de royalties.
   * `pool` = receita destinada a royalties por país, em centavos: `BR:1500000,PT:200000`.
   */
  app.get('/admin/reports/usage', { preHandler: admin }, async (req, reply) => {
    const { period, pool } = z.object({ period: z.string().regex(/^\d{4}-\d{2}$/), pool: z.string().default('') }).parse(req.query);
    const [y, m] = period.split('-').map(Number) as [number, number];
    const events = await repos.plays.allSince(Date.UTC(y, m - 1, 1), Date.UTC(y, m, 1));
    const tracks = new Map((await catalog.tracks([...new Set(events.map((e) => e.trackId))])).map((t) => [t.id, t]));
    const royaltyPoolCents = new Map(
      pool
        .split(',')
        .filter(Boolean)
        .map((p) => {
          const [c, v] = p.split(':');
          return [c!.toUpperCase(), Number(v)] as const;
        }),
    );
    const rows = usageReport({ events, tracks, royaltyPoolCents });
    return reply.header('content-type', 'text/csv; charset=utf-8').header('content-disposition', `attachment; filename="usage-${period}.csv"`).send(usageCsv(rows, period));
  });

  return app;
}
