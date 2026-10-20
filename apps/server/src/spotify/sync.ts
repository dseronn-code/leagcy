import { randomUUID } from 'node:crypto';
import {
  bestMatch,
  cleanTitle,
  HybridClock,
  keysBetween,
  occurrenceKeys,
  parseSpotifyPlaylist,
  type ChangeOp,
  type SpotifyItem,
} from '@sonora/core';
import type { CatalogService } from '../providers/catalog';
import type { ImportedItem, ImportedPlaylist, Repos } from '../repos/types';
import { SpotifyClient, SpotifyError } from './client';

/** Faixas sem fonte são procuradas de novo depois desse tempo (o acervo pode ter crescido). */
const RETRY_UNMATCHED_MS = 24 * 3_600_000;

export class SpotifyImportError extends Error {
  constructor(
    readonly code: 'invalid-link' | 'not-connected' | 'not-allowed' | 'not-found' | 'spotify-error',
    message: string,
  ) {
    super(message);
  }
}

export interface ImportSummary {
  spotifyId: string;
  localId: string;
  name: string;
  artworkUrl?: string;
  total: number;
  playable: number;
  lastSyncedAt: number;
  lastError: string | null;
}

export function summarizeImport(p: ImportedPlaylist): ImportSummary {
  return {
    spotifyId: p.spotifyId,
    localId: p.localId,
    name: p.name,
    artworkUrl: p.artworkUrl,
    total: p.items.length,
    playable: p.items.filter((i) => i.matchedTrackId).length,
    lastSyncedAt: p.lastSyncedAt,
    lastError: p.lastError,
  };
}

/**
 * Sincronização Spotify → biblioteca do Sonora (mão única).
 *
 * O servidor consulta o `snapshot_id` da playlist (barato); só quando ele muda
 * baixa as faixas, calcula o diff e grava ChangeOps no escopo `user:<id>`.
 * O app recebe essas ops pelo sync normal — adicionar ou tirar uma música no
 * Spotify aparece no Sonora sem o app precisar falar com o Spotify.
 */
export class SpotifySyncService {
  private readonly clock = new HybridClock('server-spotify');
  /** Casamentos já resolvidos (globais: dependem só do catálogo). */
  private readonly matchCache = new Map<string, { trackId: string | null; at: number }>();
  private readonly lastUserSync = new Map<string, number>();

  constructor(
    private readonly repos: Repos,
    private readonly catalog: CatalogService,
    readonly client: SpotifyClient,
  ) {}

  /** Access token válido, renovando se preciso. */
  private async accessToken(userId: string): Promise<string> {
    const link = await this.repos.spotify.getLink(userId);
    if (!link) throw new SpotifyImportError('not-connected', 'Conecte sua conta do Spotify primeiro');
    if (link.expiresAt > Date.now()) return link.accessToken;
    const t = await this.client.refresh(link.refreshToken);
    await this.repos.spotify.setLink({ ...link, accessToken: t.accessToken, refreshToken: t.refreshToken || link.refreshToken, expiresAt: t.expiresAt });
    return t.accessToken;
  }

  async connect(userId: string, code: string, codeVerifier: string, redirectUri: string) {
    const t = await this.client.exchangeCode(code, codeVerifier, redirectUri);
    const me = await this.client.me(t.accessToken);
    await this.repos.spotify.setLink({ userId, spotifyUserId: me.id, displayName: me.display_name ?? me.id, refreshToken: t.refreshToken, accessToken: t.accessToken, expiresAt: t.expiresAt });
    return { spotifyUserId: me.id, displayName: me.display_name ?? me.id };
  }

  async importPlaylist(userId: string, input: string): Promise<ImportSummary> {
    const spotifyId = parseSpotifyPlaylist(input);
    if (!spotifyId) throw new SpotifyImportError('invalid-link', 'Link de playlist do Spotify inválido');
    const existing = await this.repos.spotify.getImport(userId, spotifyId);
    if (existing) return summarizeImport(await this.syncOne(existing, true));
    const fresh: ImportedPlaylist = { userId, spotifyId, localId: randomUUID(), name: '', snapshotId: '', items: [], lastSyncedAt: 0, lastError: null };
    return summarizeImport(await this.syncOne(fresh, true, true));
  }

  async stopSyncing(userId: string, spotifyId: string) {
    const p = await this.repos.spotify.getImport(userId, spotifyId);
    if (!p) return;
    // A playlist continua na biblioteca, agora como playlist comum e editável.
    await this.write(userId, [{ entity: 'playlist', entityId: p.localId, fields: { kind: 'user', description: `Importada do Spotify (sincronização desligada)`, updatedAt: Date.now() } }]);
    await this.repos.spotify.deleteImport(userId, spotifyId);
  }

  /** Sincroniza todas as playlists do usuário (o app chama ao abrir; limitado a 1x a cada 20 s). */
  async syncUser(userId: string): Promise<ImportSummary[]> {
    const last = this.lastUserSync.get(userId) ?? 0;
    const list = await this.repos.spotify.listImports(userId);
    if (Date.now() - last < 20_000) return list.map(summarizeImport);
    this.lastUserSync.set(userId, Date.now());
    const out: ImportSummary[] = [];
    for (const p of list) out.push(summarizeImport(await this.syncOne(p, false)));
    return out;
  }

  /** Varredura periódica de todas as playlists importadas (job do servidor). */
  async syncAll(): Promise<number> {
    let changed = 0;
    for (const p of await this.repos.spotify.allImports()) {
      const before = p.snapshotId;
      const after = await this.syncOne(p, false);
      if (after.snapshotId !== before) changed++;
    }
    return changed;
  }

  async syncOne(p: ImportedPlaylist, force: boolean, isNew = false): Promise<ImportedPlaylist> {
    try {
      const token = await this.accessToken(p.userId);
      const meta = await this.client.playlist(p.spotifyId, token);
      const retryDue = p.items.some((i) => !i.matchedTrackId && Date.now() - i.matchCheckedAt > RETRY_UNMATCHED_MS);
      if (!force && meta.snapshotId === p.snapshotId && !retryDue) {
        const ok = { ...p, lastSyncedAt: Date.now(), lastError: null };
        await this.repos.spotify.saveImport(ok);
        return ok;
      }
      const remote = await this.client.items(p.spotifyId, token);
      const next = await this.reconcile(p, remote, meta, isNew);
      await this.repos.spotify.saveImport(next);
      return next;
    } catch (e) {
      const err = this.describe(e);
      if (isNew) throw err;
      const failed = { ...p, lastSyncedAt: Date.now(), lastError: err.message };
      await this.repos.spotify.saveImport(failed);
      return failed;
    }
  }

  private describe(e: unknown): SpotifyImportError {
    if (e instanceof SpotifyImportError) return e;
    if (e instanceof SpotifyError) {
      if (e.status === 403) return new SpotifyImportError('not-allowed', 'O Spotify só libera playlists que você criou ou em que colabora');
      if (e.status === 404) return new SpotifyImportError('not-found', 'Playlist não encontrada ou privada');
      if (e.status === 401) return new SpotifyImportError('not-connected', 'Conexão com o Spotify expirou; conecte de novo');
    }
    return new SpotifyImportError('spotify-error', `Falha ao falar com o Spotify: ${(e as Error).message}`);
  }

  /** Calcula o novo estado e grava as ops na biblioteca do usuário. */
  private async reconcile(p: ImportedPlaylist, remote: SpotifyItem[], meta: { name: string; snapshotId: string; artworkUrl?: string; ownerName: string }, isNew: boolean): Promise<ImportedPlaylist> {
    await this.repos.spotify.putTracks(remote);
    const keys = occurrenceKeys(remote);
    const prevByKey = new Map(p.items.map((i) => [i.key, i]));
    const positions = keysBetween(null, null, remote.length);
    const changes: Array<{ entity: 'playlist' | 'playlistEntry'; entityId: string; fields: Record<string, unknown> }> = [];
    const items: ImportedItem[] = [];

    for (let i = 0; i < remote.length; i++) {
      const it = remote[i]!;
      const key = keys[i]!;
      const prev = prevByKey.get(key);
      let matchedTrackId = prev?.matchedTrackId ?? null;
      let matchCheckedAt = prev?.matchCheckedAt ?? 0;
      if (!prev || (!matchedTrackId && Date.now() - matchCheckedAt > RETRY_UNMATCHED_MS)) {
        matchedTrackId = await this.match(it);
        matchCheckedAt = Date.now();
      }
      const trackId = matchedTrackId ?? `spotify:${it.spotifyId}`;
      const position = positions[i]!;
      const entryId = `${p.localId}/sp-${key.replace('#', '-')}`;
      if (!prev) {
        changes.push({ entity: 'playlistEntry', entityId: entryId, fields: { $exists: true, trackId, position, addedBy: 'spotify', addedAt: it.addedAt || Date.now() } });
      } else {
        const fields: Record<string, unknown> = {};
        if ((prev.matchedTrackId ?? `spotify:${it.spotifyId}`) !== trackId) fields.trackId = trackId;
        if (prev.position !== position) fields.position = position;
        if (Object.keys(fields).length) changes.push({ entity: 'playlistEntry', entityId: entryId, fields });
      }
      items.push({ ...it, key, matchedTrackId, matchCheckedAt, position });
    }

    const remoteKeys = new Set(keys);
    for (const old of p.items) {
      if (!remoteKeys.has(old.key)) changes.push({ entity: 'playlistEntry', entityId: `${p.localId}/sp-${old.key.replace('#', '-')}`, fields: { $exists: false } });
    }

    const header: Record<string, unknown> = { updatedAt: Date.now() };
    if (isNew) Object.assign(header, { $exists: true, kind: 'spotify', ownerId: p.userId, isPublic: false, spotifyId: p.spotifyId });
    if (isNew || meta.name !== p.name) header.name = meta.name;
    if (isNew || meta.artworkUrl !== p.artworkUrl) header.artworkUrl = meta.artworkUrl;
    header.description = `Sincronizada do Spotify · ${meta.ownerName}`;
    if (isNew || changes.length) changes.unshift({ entity: 'playlist', entityId: p.localId, fields: header });
    if (changes.length) await this.write(p.userId, changes);

    return { ...p, name: meta.name, artworkUrl: meta.artworkUrl, snapshotId: meta.snapshotId, items, lastSyncedAt: Date.now(), lastError: null };
  }

  /** Procura a faixa nas fontes legais configuradas (acervo Subsonic, Jamendo...). */
  private async match(it: SpotifyItem): Promise<string | null> {
    const cached = this.matchCache.get(it.spotifyId);
    if (cached && (cached.trackId || Date.now() - cached.at < RETRY_UNMATCHED_MS)) return cached.trackId;
    const base = { genres: [], excludeGenres: [], types: ['track' as const] };
    let found = null;
    for (const q of [
      { ...base, text: cleanTitle(it.title), artist: it.artists[0] },
      { ...base, text: `${cleanTitle(it.title)} ${it.artists[0] ?? ''}`.trim() },
    ]) {
      const r = await this.catalog.search(q, 10).catch(() => null);
      found = r && bestMatch(it, r.tracks);
      if (found) break;
    }
    const trackId = found?.track.id ?? null;
    this.matchCache.set(it.spotifyId, { trackId, at: Date.now() });
    return trackId;
  }

  private async write(userId: string, changes: Array<{ entity: ChangeOp['entity']; entityId: string; fields: Record<string, unknown> }>) {
    const ops: ChangeOp[] = [];
    for (const c of changes) for (const [field, value] of Object.entries(c.fields)) ops.push({ entity: c.entity, entityId: c.entityId, field, value, hlc: this.clock.tick(), deviceId: 'server' });
    await this.repos.sync.append(`user:${userId}`, userId, ops);
  }
}
