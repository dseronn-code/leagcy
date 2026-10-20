import { normalize, type Artist, type ArtistPage, type SearchQuery, type Track } from '@sonora/core';
import type { CatalogProvider, SearchResults } from './types';

/**
 * Federa vários providers: roteia por prefixo de ID, faz busca em paralelo,
 * aplica os filtros avançados que o provider não suporta nativamente e
 * deduplica faixas por ISRC (ou título+artista), mantendo a de maior fidelidade.
 */
export class CatalogService {
  private readonly byId: Map<string, CatalogProvider>;
  private readonly cache = new Map<string, { at: number; value: unknown }>();

  constructor(
    readonly providers: CatalogProvider[],
    private readonly ttlMs = 10 * 60_000,
  ) {
    this.byId = new Map(providers.map((p) => [p.id, p]));
  }

  private route(fullId: string): [CatalogProvider, string] {
    const i = fullId.indexOf(':');
    const p = this.byId.get(fullId.slice(0, i));
    if (i < 0 || !p) throw new Error(`provider desconhecido para ${fullId}`);
    return [p, fullId.slice(i + 1)];
  }

  private async cached<T>(key: string, fn: () => Promise<T>): Promise<T> {
    const hit = this.cache.get(key);
    if (hit && Date.now() - hit.at < this.ttlMs) return hit.value as T;
    const value = await fn();
    this.cache.set(key, { at: Date.now(), value });
    if (this.cache.size > 5000) this.cache.delete(this.cache.keys().next().value!);
    return value;
  }

  track(id: string): Promise<Track | null> {
    const [p, raw] = this.route(id);
    return this.cached(`t:${id}`, () => p.track(raw));
  }

  async tracks(ids: string[]): Promise<Track[]> {
    const out = await Promise.all(ids.map((id) => this.track(id).catch(() => null)));
    return out.filter((t): t is Track => t !== null);
  }

  album(id: string) {
    const [p, raw] = this.route(id);
    return this.cached(`al:${id}`, () => p.album(raw));
  }

  artist(id: string): Promise<ArtistPage | null> {
    const [p, raw] = this.route(id);
    return this.cached(`ar:${id}`, () => p.artist(raw));
  }

  async tracksByGenre(genre: string, limit: number): Promise<Track[]> {
    const all = await Promise.allSettled(this.providers.map((p) => this.cached(`g:${p.id}:${genre}:${limit}`, () => p.tracksByGenre(genre, limit))));
    return all.flatMap((r) => (r.status === 'fulfilled' ? r.value : []));
  }

  async search(q: SearchQuery, limit = 30): Promise<SearchResults & { errors: string[] }> {
    const settled = await Promise.allSettled(this.providers.map((p) => p.search(q, limit)));
    const errors: string[] = [];
    const merged: SearchResults = { tracks: [], albums: [], artists: [] };
    settled.forEach((r, i) => {
      if (r.status === 'rejected') errors.push(`${this.providers[i]!.id}: ${String(r.reason)}`);
      else {
        merged.tracks.push(...r.value.tracks);
        merged.albums.push(...r.value.albums);
        merged.artists.push(...r.value.artists);
      }
    });
    const tracks = rankTracks(dedupeTracks(merged.tracks.filter((t) => matchesFilters(t, q))), q).slice(0, limit);
    const albums = merged.albums.filter((a) => yearOk(a.year, q) && genreOk(a.genres, q)).slice(0, limit);
    const artists = dedupeBy(merged.artists, (a: Artist) => normalize(a.name)).slice(0, limit);
    const wanted = new Set(q.types);
    return {
      tracks: wanted.size === 0 || wanted.has('track') ? tracks : [],
      albums: wanted.size === 0 || wanted.has('album') ? albums : [],
      artists: wanted.size === 0 || wanted.has('artist') ? artists : [],
      errors,
    };
  }
}

function yearOk(year: number | undefined, q: SearchQuery) {
  if (q.yearFrom === undefined) return true;
  return year !== undefined && year >= q.yearFrom && year <= (q.yearTo ?? q.yearFrom);
}

function genreOk(genres: string[], q: SearchQuery) {
  const g = genres.map((x) => x.toLowerCase());
  if (q.excludeGenres.some((x) => g.includes(x))) return false;
  return q.genres.length === 0 || q.genres.some((x) => g.includes(x));
}

export function matchesFilters(t: Track, q: SearchQuery): boolean {
  if (!yearOk(t.year, q) || !genreOk(t.genres, q)) return false;
  if (q.artist && !t.artists.some((a) => normalize(a.name).includes(normalize(q.artist!)))) return false;
  if (q.album && !normalize(t.albumTitle).includes(normalize(q.album))) return false;
  if (q.minPopularity !== undefined && (t.popularity ?? 0) < q.minPopularity) return false;
  if (q.maxPopularity !== undefined && (t.popularity ?? 0) > q.maxPopularity) return false;
  if (q.quality === 'spatial' && !t.sources.some((s) => s.spatial)) return false;
  if (q.quality === 'hires' && !t.sources.some((s) => (s.bitDepth ?? 16) > 16 || s.sampleRateHz > 48_000)) return false;
  if (q.quality === 'lossless' && !t.sources.some((s) => ['flac', 'alac', 'wav'].includes(s.codec))) return false;
  return true;
}

function fidelity(t: Track) {
  return Math.max(0, ...t.sources.map((s) => (['flac', 'alac', 'wav'].includes(s.codec) ? 1e6 : 0) + (s.bitDepth ?? 16) * s.sampleRateHz / 1000));
}

export function dedupeTracks(tracks: Track[]): Track[] {
  const best = new Map<string, Track>();
  for (const t of tracks) {
    const k = t.isrc ?? `${normalize(t.title)}|${normalize(t.artists[0]?.name ?? '')}`;
    const cur = best.get(k);
    if (!cur || fidelity(t) > fidelity(cur)) best.set(k, t);
  }
  return [...best.values()];
}

function rankTracks(tracks: Track[], q: SearchQuery): Track[] {
  const text = normalize(q.text);
  const score = (t: Track) => {
    const title = normalize(t.title);
    const exact = text && title === text ? 2 : text && title.startsWith(text) ? 1 : 0;
    return exact + (t.popularity ?? 0) / 100;
  };
  return tracks.map((t, i) => ({ t, s: score(t), i })).sort((a, b) => b.s - a.s || a.i - b.i).map((x) => x.t);
}

function dedupeBy<T>(items: T[], key: (t: T) => string): T[] {
  const seen = new Set<string>();
  return items.filter((x) => (seen.has(key(x)) ? false : (seen.add(key(x)), true)));
}

