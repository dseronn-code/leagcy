import type { Album, AlbumType, ArtistPage, SearchQuery, Track } from '@sonora/core';
import { getJson, ProviderError, type CatalogProvider, type SearchResults } from './types';

/**
 * Jamendo: ~600 mil faixas Creative Commons, com stream em FLAC.
 * Docs: https://developer.jamendo.com/v3.0
 */

interface JTrack {
  id: string;
  name: string;
  duration: number;
  artist_id: string;
  artist_name: string;
  album_id: string;
  album_name: string;
  album_image?: string;
  image?: string;
  position?: number;
  releasedate?: string;
  audio: string;
  musicinfo?: { tags?: { genres?: string[] } };
  lyrics?: string;
}

interface JAlbum {
  id: string;
  name: string;
  artist_id: string;
  artist_name: string;
  image?: string;
  releasedate?: string;
  tracks?: JTrack[];
}

interface JArtist {
  id: string;
  name: string;
  image?: string;
}

interface JResponse<T> {
  headers: { status: string; error_message?: string };
  results: T[];
}

const P = 'jamendo';
const id = (raw: string) => `${P}:${raw}`;

export class JamendoProvider implements CatalogProvider {
  readonly id = P;
  private readonly base = 'https://api.jamendo.com/v3.0';

  constructor(private readonly clientId: string) {}

  private async call<T>(path: string, params: Record<string, string | number | undefined>): Promise<T[]> {
    const u = new URL(`${this.base}/${path}`);
    u.searchParams.set('client_id', this.clientId);
    u.searchParams.set('format', 'json');
    for (const [k, v] of Object.entries(params)) if (v !== undefined && v !== '') u.searchParams.set(k, String(v));
    const r = await getJson<JResponse<T>>(P, u.toString());
    if (r.headers.status !== 'success') throw new ProviderError(P, r.headers.error_message ?? 'erro');
    return r.results;
  }

  private mapTrack = (t: JTrack): Track => {
    const flac = t.audio.replace(/format=mp3\d?/, 'format=flac');
    return {
      id: id(t.id),
      provider: P,
      title: t.name,
      artists: [{ id: id(t.artist_id), name: t.artist_name }],
      albumId: id(t.album_id),
      albumTitle: t.album_name,
      artworkUrl: t.album_image || t.image,
      durationMs: t.duration * 1000,
      trackNumber: t.position,
      year: t.releasedate ? Number(t.releasedate.slice(0, 4)) : undefined,
      genres: t.musicinfo?.tags?.genres ?? [],
      sources: [
        { url: flac, codec: 'flac', bitrate: 900, sampleRateHz: 44_100, bitDepth: 16, channels: 2 },
        { url: t.audio, codec: 'mp3', bitrate: 192, sampleRateHz: 44_100, channels: 2 },
      ],
    };
  };

  private mapAlbum = (a: JAlbum): Album => ({
    id: id(a.id),
    provider: P,
    title: a.name,
    artists: [{ id: id(a.artist_id), name: a.artist_name }],
    type: guessType(a.tracks?.length),
    year: a.releasedate ? Number(a.releasedate.slice(0, 4)) : undefined,
    releaseDate: a.releasedate,
    artworkUrl: a.image,
    genres: [],
    trackIds: (a.tracks ?? []).map((t) => id(t.id)),
    bestQuality: { codec: 'flac', sampleRateHz: 44_100, bitDepth: 16 },
  });

  async search(q: SearchQuery, limit: number): Promise<SearchResults> {
    const text = [q.text, q.lyrics].filter(Boolean).join(' ');
    const dateRange = q.yearFrom ? `${q.yearFrom}-01-01_${q.yearTo ?? q.yearFrom}-12-31` : undefined;
    const [tracks, albums, artists] = await Promise.all([
      this.call<JTrack>('tracks/', {
        search: text || undefined,
        artist_name: q.artist,
        album_name: q.album,
        tags: q.genres.join('+') || undefined,
        datebetween: dateRange,
        include: 'musicinfo',
        audioformat: 'mp32',
        order: 'popularity_total',
        limit,
      }),
      q.text ? this.call<JAlbum>('albums/', { namesearch: q.text, limit: Math.min(limit, 20) }) : Promise.resolve([]),
      q.text || q.artist ? this.call<JArtist>('artists/', { namesearch: q.artist ?? q.text, limit: Math.min(limit, 20) }) : Promise.resolve([]),
    ]);
    return {
      tracks: tracks.map(this.mapTrack),
      albums: albums.map(this.mapAlbum),
      artists: artists.map((a) => ({ id: id(a.id), provider: P, name: a.name, imageUrl: a.image, genres: [] })),
    };
  }

  async track(rawId: string) {
    const [t] = await this.call<JTrack>('tracks/', { id: rawId, include: 'musicinfo', audioformat: 'mp32' });
    return t ? this.mapTrack(t) : null;
  }

  async album(rawId: string) {
    const [a] = await this.call<JAlbum>('albums/tracks/', { id: rawId, audioformat: 'mp32' });
    if (!a) return null;
    const tracks = (a.tracks ?? []).map((t) =>
      this.mapTrack({ ...t, artist_id: a.artist_id, artist_name: a.artist_name, album_id: a.id, album_name: a.name, album_image: a.image }),
    );
    return { album: this.mapAlbum(a), tracks };
  }

  async artist(rawId: string): Promise<ArtistPage | null> {
    const [[a], albums, top] = await Promise.all([
      this.call<JArtist>('artists/', { id: rawId }),
      this.call<JAlbum>('albums/', { artist_id: rawId, limit: 50, order: 'releasedate_desc' }),
      this.call<JTrack>('tracks/', { artist_id: rawId, order: 'popularity_total', limit: 10, include: 'musicinfo', audioformat: 'mp32' }),
    ]);
    if (!a) return null;
    const mapped = albums.map(this.mapAlbum);
    const topTracks = top.map(this.mapTrack);
    const genres = [...new Set(topTracks.flatMap((t) => t.genres))];
    // Jamendo não expõe "artistas relacionados": aproximamos por gênero principal.
    const related = genres[0]
      ? (await this.call<JTrack>('tracks/', { tags: genres[0], order: 'popularity_week', limit: 30 }))
          .filter((t) => t.artist_id !== rawId)
          .reduce<ArtistPage['related']>((acc, t) => {
            if (!acc.some((r) => r.id === id(t.artist_id))) acc.push({ id: id(t.artist_id), provider: P, name: t.artist_name, genres: [genres[0]!] });
            return acc;
          }, [])
          .slice(0, 10)
      : [];
    return {
      artist: { id: id(a.id), provider: P, name: a.name, imageUrl: a.image, genres },
      topTracks,
      albums: mapped.filter((x) => x.type === 'album' || x.type === 'ep'),
      singles: mapped.filter((x) => x.type === 'single'),
      compilations: mapped.filter((x) => x.type === 'compilation'),
      related,
    };
  }

  async tracksByGenre(genre: string, limit: number) {
    return (await this.call<JTrack>('tracks/', { tags: genre, order: 'popularity_month', limit, include: 'musicinfo', audioformat: 'mp32' })).map(this.mapTrack);
  }
}

function guessType(trackCount: number | undefined): AlbumType {
  if (trackCount === undefined) return 'album';
  if (trackCount <= 3) return 'single';
  if (trackCount <= 6) return 'ep';
  return 'album';
}
