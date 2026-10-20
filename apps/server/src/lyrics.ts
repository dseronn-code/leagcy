import type { Track } from '@sonora/core';

/**
 * Letras via LRCLIB (https://lrclib.net) — base aberta de letras sincronizadas.
 * Resultados ficam em cache (memória aqui; tabela lyrics_cache no Postgres em produção).
 */
export interface LyricsResult {
  synced: string | null;
  plain: string | null;
  source: 'lrclib' | 'none';
}

interface LrcLibRecord {
  syncedLyrics?: string | null;
  plainLyrics?: string | null;
  instrumental?: boolean;
}

export class LyricsService {
  private cache = new Map<string, LyricsResult>();

  constructor(private readonly base = 'https://lrclib.net/api') {}

  async forTrack(t: Track): Promise<LyricsResult> {
    const hit = this.cache.get(t.id);
    if (hit) return hit;
    const u = new URL(`${this.base}/get`);
    u.searchParams.set('track_name', t.title);
    u.searchParams.set('artist_name', t.artists[0]?.name ?? '');
    u.searchParams.set('album_name', t.albumTitle);
    u.searchParams.set('duration', String(Math.round(t.durationMs / 1000)));
    let result: LyricsResult = { synced: null, plain: null, source: 'none' };
    try {
      const res = await fetch(u, { headers: { 'user-agent': 'Sonora/0.1 (https://github.com/dseronn-code/leagcy)' }, signal: AbortSignal.timeout(6000) });
      if (res.ok) {
        const r = (await res.json()) as LrcLibRecord;
        if (!r.instrumental) result = { synced: r.syncedLyrics ?? null, plain: r.plainLyrics ?? null, source: 'lrclib' };
      }
    } catch {
      // Sem letra é um estado válido; não falha a requisição.
    }
    this.cache.set(t.id, result);
    return result;
  }

  /** Busca por trecho de letra; retorna pares artista/título para resolver no catálogo. */
  async searchByLyrics(snippet: string): Promise<Array<{ title: string; artist: string; album: string }>> {
    const u = new URL(`${this.base}/search`);
    u.searchParams.set('q', snippet);
    try {
      const res = await fetch(u, { signal: AbortSignal.timeout(6000) });
      if (!res.ok) return [];
      const rows = (await res.json()) as Array<{ trackName: string; artistName: string; albumName: string }>;
      return rows.slice(0, 20).map((r) => ({ title: r.trackName, artist: r.artistName, album: r.albumName }));
    } catch {
      return [];
    }
  }
}
