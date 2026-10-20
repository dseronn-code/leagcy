import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import { applyOps, compareEntries, materialize, type Album, type ArtistPage, type ChangeOp, type SearchQuery, type Track } from '@sonora/core';
import { buildApp } from '../src/app';
import { LyricsService } from '../src/lyrics';
import { CatalogService } from '../src/providers/catalog';
import { SpotifyImportProvider } from '../src/providers/spotifyImport';
import type { CatalogProvider } from '../src/providers/types';
import { RecommendationService } from '../src/recs/service';
import { memoryRepos } from '../src/repos/memory';
import { SpotifyClient } from '../src/spotify/client';
import { SpotifySyncService } from '../src/spotify/sync';

// ---------- Acervo próprio (fonte legal de áudio) ----------
const LIB: Track[] = [
  ['l1', 'Águas de Março', 'Elis Regina', 211_000],
  ['l2', 'Garota de Ipanema', 'Tom Jobim', 323_000],
  ['l3', 'Construção', 'Chico Buarque', 384_000],
].map(([id, title, artist, ms]) => ({
  id: `own:${id}`,
  provider: 'own',
  title: title as string,
  artists: [{ id: `own:${artist}`, name: artist as string }],
  albumId: 'own:al',
  albumTitle: 'Acervo',
  durationMs: ms as number,
  genres: ['mpb'],
  sources: [{ url: `https://nas.local/${id}.flac`, codec: 'flac', bitrate: 4000, sampleRateHz: 96_000, bitDepth: 24, channels: 2 }],
}));

class OwnLibrary implements CatalogProvider {
  readonly id = 'own';
  async search(q: SearchQuery) {
    const words = q.text.toLowerCase().normalize('NFD').replace(/\p{Diacritic}/gu, '').split(' ').filter(Boolean);
    const hits = LIB.filter((t) => words.every((w) => `${t.title} ${t.artists[0]!.name}`.toLowerCase().normalize('NFD').replace(/\p{Diacritic}/gu, '').includes(w)));
    return { tracks: hits, albums: [] as Album[], artists: [] };
  }
  async track(raw: string) {
    return LIB.find((t) => t.id === `own:${raw}`) ?? null;
  }
  async album() {
    return null;
  }
  async artist(): Promise<ArtistPage | null> {
    return null;
  }
  async tracksByGenre() {
    return [];
  }
}

// ---------- API do Spotify falsa ----------
const PLAYLIST_ID = '37i9dQZF1DXcBWIGoYBM5M';
const sp = (id: string, name: string, artist: string, ms: number) => ({ id, name, artists: [{ name: artist }], album: { name: 'X', images: [{ url: `https://img/${id}` }] }, duration_ms: ms, type: 'track' });
const state = {
  snapshot: 1,
  tracks: [sp('s1aaaaaaaaaaaaaaaaaaaa', 'Águas de Março - Remastered 2004', 'Elis Regina', 212_000), sp('s2aaaaaaaaaaaaaaaaaaaa', 'Bohemian Rhapsody', 'Queen', 354_000)],
  calls: { items: 0 },
};
const json = (body: unknown, status = 200) => new Response(JSON.stringify(body), { status, headers: { 'content-type': 'application/json' } });

const fakeFetch = async (url: string, init?: RequestInit) => {
  const u = new URL(url);
  if (u.pathname === '/api/token') {
    const form = new URLSearchParams(String(init?.body));
    if (form.get('grant_type') === 'authorization_code' && form.get('code') !== 'good-code') return json({ error_description: 'invalid code' }, 400);
    return json({ access_token: 'at', refresh_token: 'rt', expires_in: 3600 });
  }
  if (u.pathname === '/v1/me') return json({ id: 'dseronn', display_name: 'Dseronn' });
  if (u.pathname === `/v1/playlists/${PLAYLIST_ID}`) return json({ id: PLAYLIST_ID, name: 'Minha playlist', snapshot_id: `snap-${state.snapshot}`, images: [{ url: 'https://img/pl' }], owner: { id: 'dseronn', display_name: 'Dseronn' } });
  if (u.pathname === `/v1/playlists/${PLAYLIST_ID}/items`) {
    state.calls.items++;
    const offset = Number(u.searchParams.get('offset'));
    // Formato novo da API (fev/2026): a faixa vem em `item`.
    return json({ items: state.tracks.slice(offset, offset + 50).map((t) => ({ added_at: '2026-10-01T00:00:00Z', item: t })), next: null });
  }
  if (u.pathname.startsWith('/v1/playlists/')) return json({ error: { message: 'Forbidden' } }, 403);
  return json({}, 404);
};

const repos = memoryRepos();
const catalog = new CatalogService([new OwnLibrary(), new SpotifyImportProvider(repos)]);
const spotify = new SpotifySyncService(repos, catalog, new SpotifyClient('client-id', fakeFetch as typeof fetch, 'https://accounts.test', 'https://api.test/v1'));
let app: Awaited<ReturnType<typeof buildApp>>;
let h: Record<string, string>;
let me: { id: string };

/** Lê a playlist como o app vê: aplicando as ops do sync. */
async function libraryPlaylist(localId: string) {
  const ops = (await app.inject({ url: `/sync/pull?scope=user:${me.id}`, headers: h })).json().ops as ChangeOp[];
  const store = applyOps(new Map(), ops);
  const pl = materialize(store, 'playlist').find((p) => p.id === localId)!;
  const entries = materialize(store, 'playlistEntry')
    .filter((e) => e.id.startsWith(`${localId}/`))
    .map((e) => ({ entryId: e.id, position: e.position as string, trackId: e.trackId as string }))
    .sort(compareEntries);
  return { pl, trackIds: entries.map((e) => e.trackId) };
}

beforeAll(async () => {
  app = await buildApp({ repos, catalog, lyrics: new LyricsService('http://127.0.0.1:9'), recs: new RecommendationService(repos, catalog), spotify, jwtSecret: 'test' });
  const r = await app.inject({ method: 'POST', url: '/auth/register', payload: { email: 'sp@x.com', password: 'senha-forte-123' } });
  h = { authorization: `Bearer ${r.json().token}` };
  me = (await app.inject({ url: '/me', headers: h })).json();
});
afterAll(() => app.close());

describe('importação de playlist do Spotify', () => {
  it('exige conectar a conta antes de importar', async () => {
    const r = await app.inject({ method: 'POST', url: '/spotify/playlists', headers: h, payload: { url: `https://open.spotify.com/playlist/${PLAYLIST_ID}` } });
    expect(r.statusCode).toBe(409);
    const bad = await app.inject({ method: 'POST', url: '/spotify/connect', headers: h, payload: { code: 'bad', codeVerifier: 'v'.repeat(64), redirectUri: 'sonora://spotify-callback' } });
    expect(bad.statusCode).toBe(400);
    const ok = await app.inject({ method: 'POST', url: '/spotify/connect', headers: h, payload: { code: 'good-code', codeVerifier: 'v'.repeat(64), redirectUri: 'sonora://spotify-callback' } });
    expect(ok.json().account.displayName).toBe('Dseronn');
    expect((await app.inject({ url: '/spotify/status', headers: h })).json()).toMatchObject({ enabled: true, connected: true });
  });

  it('rejeita links inválidos e playlists de terceiros', async () => {
    expect((await app.inject({ method: 'POST', url: '/spotify/playlists', headers: h, payload: { url: 'https://open.spotify.com/album/abc' } })).statusCode).toBe(400);
    const other = await app.inject({ method: 'POST', url: '/spotify/playlists', headers: h, payload: { url: 'spotify:playlist:0000000000000000000000' } });
    expect(other.statusCode).toBe(403);
  });

  let localId = '';
  it('importa: casa o que existe no acervo e mostra o resto como "não encontrada"', async () => {
    const r = await app.inject({ method: 'POST', url: '/spotify/playlists', headers: h, payload: { url: `https://open.spotify.com/intl-pt/playlist/${PLAYLIST_ID}?si=xyz` } });
    expect(r.statusCode).toBe(200);
    const p = r.json().playlist;
    expect(p).toMatchObject({ name: 'Minha playlist', total: 2, playable: 1 });
    localId = p.localId;
    const lib = await libraryPlaylist(localId);
    expect(lib.pl).toMatchObject({ name: 'Minha playlist', kind: 'spotify' });
    expect(lib.trackIds).toEqual(['own:l1', 'spotify:s2aaaaaaaaaaaaaaaaaaaa']);
    // A faixa sem fonte ainda tem metadados para exibir.
    const t = (await app.inject({ url: '/tracks/spotify:s2aaaaaaaaaaaaaaaaaaaa', headers: h })).json();
    expect(t).toMatchObject({ title: 'Bohemian Rhapsody', sources: [] });
  });

  it('sem mudança no Spotify (mesmo snapshot) não baixa as faixas de novo', async () => {
    const before = state.calls.items;
    await spotify.syncAll();
    expect(state.calls.items).toBe(before);
  });

  it('adicionar, tirar e reordenar no Spotify reflete na biblioteca', async () => {
    state.tracks = [sp('s3aaaaaaaaaaaaaaaaaaaa', 'Construção', 'Chico Buarque', 384_000), state.tracks[0]!];
    state.snapshot++;
    expect(await spotify.syncAll()).toBe(1);
    const lib = await libraryPlaylist(localId);
    expect(lib.trackIds).toEqual(['own:l3', 'own:l1']);
    const summary = (await app.inject({ url: '/spotify/playlists', headers: h })).json().playlists[0];
    expect(summary).toMatchObject({ total: 2, playable: 2, lastError: null });
  });

  it('parar de sincronizar mantém a playlist como playlist comum', async () => {
    await app.inject({ method: 'DELETE', url: `/spotify/playlists/${PLAYLIST_ID}`, headers: h });
    expect((await app.inject({ url: '/spotify/playlists', headers: h })).json().playlists).toEqual([]);
    const lib = await libraryPlaylist(localId);
    expect(lib.pl.kind).toBe('user');
    expect(lib.trackIds).toEqual(['own:l3', 'own:l1']);
  });
});
