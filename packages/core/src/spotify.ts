import type { Track } from './types';
import { normalize } from './search';

/**
 * Importação de playlists do Spotify (somente metadados) e casamento das faixas
 * com fontes de áudio legais (acervo próprio via Subsonic, Creative Commons...).
 */

export interface SpotifyItem {
  spotifyId: string;
  title: string;
  artists: string[];
  album: string;
  durationMs: number;
  isrc?: string;
  artworkUrl?: string;
  addedAt: number;
}

const ID = /^[A-Za-z0-9]{22}$/;

/**
 * Extrai o ID de uma playlist a partir do que o usuário colar:
 *   https://open.spotify.com/playlist/37i9dQZF1DXcBWIGoYBM5M?si=...
 *   https://open.spotify.com/intl-pt/playlist/37i9...
 *   spotify:playlist:37i9...
 *   37i9... (só o ID)
 */
export function parseSpotifyPlaylist(input: string): string | null {
  const s = input.trim();
  if (ID.test(s)) return s;
  const uri = /^spotify:(?:user:[^:]+:)?playlist:([A-Za-z0-9]{22})$/.exec(s);
  if (uri) return uri[1]!;
  try {
    const u = new URL(s);
    if (!/(^|\.)spotify\.com$/.test(u.hostname)) return null;
    const parts = u.pathname.split('/').filter(Boolean);
    const i = parts.indexOf('playlist');
    const id = i >= 0 ? parts[i + 1] : undefined;
    return id && ID.test(id) ? id : null;
  } catch {
    return null;
  }
}

export interface PlaylistDiff {
  added: SpotifyItem[];
  removed: string[];
  /** true se a ordem das faixas que permaneceram mudou. */
  reordered: boolean;
}

/**
 * Compara duas versões da playlist. Uma mesma faixa pode aparecer mais de uma
 * vez; cada ocorrência é identificada por `spotifyId#n` (ver occurrenceKeys).
 */
export function diffPlaylist(prev: SpotifyItem[], next: SpotifyItem[]): PlaylistDiff {
  const pk = occurrenceKeys(prev);
  const nk = occurrenceKeys(next);
  const prevSet = new Set(pk);
  const nextSet = new Set(nk);
  const added = next.filter((_, i) => !prevSet.has(nk[i]!));
  const removed = pk.filter((k) => !nextSet.has(k));
  const keptPrev = pk.filter((k) => nextSet.has(k));
  const keptNext = nk.filter((k) => prevSet.has(k));
  return { added, removed, reordered: keptPrev.some((k, i) => k !== keptNext[i]) };
}

/** Chave estável por ocorrência: a 2ª vez que a mesma faixa aparece vira `id#1`. */
export function occurrenceKeys(items: SpotifyItem[]): string[] {
  const seen = new Map<string, number>();
  return items.map((it) => {
    const n = seen.get(it.spotifyId) ?? 0;
    seen.set(it.spotifyId, n + 1);
    return `${it.spotifyId}#${n}`;
  });
}

/** Remove sufixos que variam entre serviços: "- Remastered 2011", "(feat. X)", "[Live]"... */
export function cleanTitle(title: string): string {
  return normalize(
    title
      .replace(/\s*[([](feat|ft|with|com)\.?\s[^)\]]*[)\]]/gi, '')
      .replace(/\s+-\s+(\d{4}\s+)?(remaster(ed)?|mono|stereo|single|radio edit|ao vivo|live|versão|version)\b.*$/i, '')
      .replace(/\s*[([](\d{4}\s+)?(remaster(ed)?|mono|stereo|deluxe|bonus track)[^)\]]*[)\]]/gi, ''),
  )
    .replace(/[^\p{L}\p{N} ]/gu, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

function tokens(s: string): Set<string> {
  return new Set(cleanTitle(s).split(' ').filter(Boolean));
}

function jaccard(a: Set<string>, b: Set<string>): number {
  if (!a.size && !b.size) return 1;
  let inter = 0;
  for (const x of a) if (b.has(x)) inter++;
  return inter / (a.size + b.size - inter);
}

/**
 * Nota 0..1 de que `candidate` é a mesma gravação de `item`.
 * ISRC igual é prova; senão combina título, artista e duração.
 */
export function matchScore(item: SpotifyItem, candidate: Track): number {
  if (item.isrc && candidate.isrc) return item.isrc.toUpperCase() === candidate.isrc.toUpperCase() ? 1 : 0;
  const title = cleanTitle(item.title) === cleanTitle(candidate.title) ? 1 : jaccard(tokens(item.title), tokens(candidate.title));
  const itemArtists = item.artists.map(normalize);
  const candArtists = candidate.artists.map((a) => normalize(a.name));
  const artist = itemArtists.some((a) => candArtists.some((c) => c === a || c.includes(a) || a.includes(c))) ? 1 : 0;
  const diff = Math.abs(item.durationMs - candidate.durationMs);
  const duration = candidate.durationMs === 0 ? 0.5 : diff <= 3000 ? 1 : diff <= 10_000 ? 0.6 : diff <= 30_000 ? 0.2 : 0;
  return 0.5 * title + 0.35 * artist + 0.15 * duration;
}

/** Abaixo disso a faixa fica como "não encontrada" (melhor do que tocar a música errada). */
export const MATCH_THRESHOLD = 0.8;

export function bestMatch(item: SpotifyItem, candidates: Track[]): { track: Track; score: number } | null {
  let best: { track: Track; score: number } | null = null;
  for (const c of candidates) {
    if (c.provider === 'spotify') continue;
    const score = matchScore(item, c);
    if (!best || score > best.score) best = { track: c, score };
  }
  return best && best.score >= MATCH_THRESHOLD ? best : null;
}
