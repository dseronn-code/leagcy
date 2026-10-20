import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import type { Album, ArtistPage, SearchQuery, Track } from '@sonora/core';
import { buildApp } from '../src/app';
import { LyricsService } from '../src/lyrics';
import { CatalogService } from '../src/providers/catalog';
import type { CatalogProvider } from '../src/providers/types';
import { rebuildNeighbors } from '../src/recs/cooccurrenceJob';
import { RecommendationService } from '../src/recs/service';
import { memoryRepos } from '../src/repos/memory';

function track(n: number, artist: string, genre: string, p: Partial<Track> = {}): Track {
  return {
    id: `fake:${n}`,
    provider: 'fake',
    title: `Faixa ${n}`,
    artists: [{ id: `fake:${artist}`, name: artist }],
    albumId: 'fake:al',
    albumTitle: 'Álbum',
    durationMs: 200_000,
    year: 1970 + n,
    genres: [genre],
    popularity: 50,
    sources: [{ url: `https://x/${n}.flac`, codec: 'flac', bitrate: 3000, sampleRateHz: n % 2 ? 192_000 : 44_100, bitDepth: n % 2 ? 24 : 16, channels: 2 }],
    ...p,
  };
}

const TRACKS = Array.from({ length: 30 }, (_, i) => track(i, `art${i % 6}`, i % 2 ? 'mpb' : 'jazz'));

class FakeProvider implements CatalogProvider {
  readonly id = 'fake';
  async search(_q: SearchQuery) {
    return { tracks: TRACKS, albums: [] as Album[], artists: [] };
  }
  async track(raw: string) {
    return TRACKS.find((t) => t.id === `fake:${raw}`) ?? null;
  }
  async album() {
    return null;
  }
  async artist(): Promise<ArtistPage | null> {
    return null;
  }
  async tracksByGenre(genre: string, limit: number) {
    return TRACKS.filter((t) => t.genres.includes(genre)).slice(0, limit);
  }
}

const repos = memoryRepos();
const catalog = new CatalogService([new FakeProvider()]);
let app: Awaited<ReturnType<typeof buildApp>>;

async function user(email: string) {
  const r = await app.inject({ method: 'POST', url: '/auth/register', payload: { email, password: 'senha-forte-123' } });
  return { authorization: `Bearer ${r.json().token}` };
}

beforeAll(async () => {
  app = await buildApp({ repos, catalog, lyrics: new LyricsService('http://127.0.0.1:9'), recs: new RecommendationService(repos, catalog), jwtSecret: 'test' });
});
afterAll(() => app.close());

describe('API', () => {
  it('exige autenticação', async () => {
    expect((await app.inject({ url: '/search?q=x' })).statusCode).toBe(401);
  });

  it('login com senha errada falha', async () => {
    await user('a@x.com');
    const r = await app.inject({ method: 'POST', url: '/auth/login', payload: { email: 'a@x.com', password: 'errada-123456' } });
    expect(r.statusCode).toBe(401);
  });

  it('busca avançada aplica filtros de ano, gênero e qualidade', async () => {
    const h = await user('b@x.com');
    const r = await app.inject({ url: `/search?q=${encodeURIComponent('genre:mpb year:1971-1980 quality:hires')}`, headers: h });
    const ids = r.json().tracks.map((t: Track) => t.id);
    expect(ids.length).toBeGreaterThan(0);
    for (const t of r.json().tracks as Track[]) {
      expect(t.genres).toContain('mpb');
      expect(t.year).toBeGreaterThanOrEqual(1971);
      expect(t.year).toBeLessThanOrEqual(1980);
    }
  });

  it('ingere plays de forma idempotente e calcula estatísticas', async () => {
    const h = await user('c@x.com');
    const now = Date.now();
    const events = Array.from({ length: 10 }, (_, i) => ({
      id: `evt-0000-${i}`,
      trackId: TRACKS[i % 4]!.id,
      artistIds: TRACKS[i % 4]!.artists.map((a) => a.id),
      genres: TRACKS[i % 4]!.genres,
      startedAt: now - i * 210_000,
      listenedMs: 180_000,
      trackDurationMs: 200_000,
      skipped: false,
      source: { type: 'library' },
      deviceId: 'phone',
      tzOffsetMin: -180,
    }));
    expect((await app.inject({ method: 'POST', url: '/plays', headers: h, payload: { events } })).json().accepted).toBe(10);
    expect((await app.inject({ method: 'POST', url: '/plays', headers: h, payload: { events } })).json().accepted).toBe(0);
    const s = (await app.inject({ url: '/stats/summary?tz=-180', headers: h })).json();
    expect(s.totalMinutes).toBe(30);
    expect(s.topTracks[0].plays).toBe(3);
    expect(s.tracks[s.topTracks[0].id].title).toMatch(/Faixa/);
    expect(s.streak.current).toBeGreaterThanOrEqual(1);
  });

  it('sincroniza ops entre dispositivos e controla acesso a playlists colaborativas', async () => {
    const owner = await user('d@x.com');
    const other = await user('e@x.com');
    const me = (await app.inject({ url: '/me', headers: owner })).json();
    const scope = `user:${me.id}`;
    const op = { entity: 'like', entityId: 'fake:1', field: '$exists', value: true, hlc: '000000001:00000:dev1', deviceId: 'dev1' };
    await app.inject({ method: 'POST', url: '/sync/push', headers: owner, payload: { scope, ops: [op, op] } });
    const pulled = (await app.inject({ url: `/sync/pull?scope=${scope}`, headers: owner })).json();
    expect(pulled.ops).toHaveLength(1);
    expect((await app.inject({ url: `/sync/pull?scope=${scope}`, headers: other })).statusCode).toBe(403);

    // playlist colaborativa
    expect((await app.inject({ method: 'POST', url: '/playlists/pl1/claim', headers: owner })).json().role).toBe('owner');
    expect((await app.inject({ method: 'POST', url: '/playlists/pl1/claim', headers: other })).statusCode).toBe(409);
    const pscope = 'playlist:pl1';
    expect((await app.inject({ method: 'POST', url: '/sync/push', headers: other, payload: { scope: pscope, ops: [] } })).statusCode).toBe(403);
    const { token } = (await app.inject({ method: 'POST', url: '/playlists/pl1/invites', headers: owner })).json();
    expect((await app.inject({ method: 'POST', url: `/invites/${token}/accept`, headers: other })).statusCode).toBe(200);
    expect((await app.inject({ method: 'POST', url: '/sync/push', headers: other, payload: { scope: pscope, ops: [{ ...op, entity: 'playlistEntry', entityId: 'pl1/e1' }] } })).statusCode).toBe(200);
    expect((await app.inject({ url: '/sync/scopes', headers: other })).json().scopes).toContain(pscope);
  });

  it('gera recomendações a partir da co-escuta', async () => {
    const h = await user('f@x.com');
    const now = Date.now();
    const mk = (i: number, t: Track, start: number) => ({ id: `rec-evt-${i}`, trackId: t.id, artistIds: t.artists.map((a) => a.id), genres: t.genres, startedAt: start, listenedMs: 190_000, trackDurationMs: 200_000, skipped: false, source: { type: 'library' }, deviceId: 'd', tzOffsetMin: 0 });
    const events = TRACKS.slice(0, 12).map((t, i) => mk(i, t, now - 86_400_000 + i * 200_000));
    await app.inject({ method: 'POST', url: '/plays', headers: h, payload: { events } });
    await rebuildNeighbors(repos);
    expect((await repos.neighbors.of(TRACKS[0]!.id, 5)).length).toBeGreaterThan(0);

    const radio = (await app.inject({ url: `/recs/radio?seed=${TRACKS[0]!.id}`, headers: h })).json();
    expect(radio.tracks.length).toBeGreaterThan(0);
    expect(radio.tracks.map((t: Track) => t.id)).not.toContain(TRACKS[0]!.id);

    const weekly = (await app.inject({ url: '/recs/discover-weekly', headers: h })).json();
    const played = new Set(events.map((e) => e.trackId));
    expect(weekly.tracks.every((t: Track) => !played.has(t.id))).toBe(true);

    const mixes = (await app.inject({ url: '/recs/daily-mixes', headers: h })).json().mixes;
    expect(mixes.length).toBeGreaterThan(0);
  });
});
