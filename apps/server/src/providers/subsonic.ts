import { createHash, randomBytes } from 'node:crypto';
import type { Album, ArtistPage, AudioSource, Codec, SearchQuery, Track } from '@sonora/core';
import { getJson, ProviderError, type CatalogProvider, type SearchResults } from './types';

/**
 * Acervo próprio via API Subsonic/OpenSubsonic (Navidrome, Gonic, Airsonic...).
 * É a rota para o máximo de fidelidade: `format=raw` entrega o arquivo original
 * (FLAC/ALAC 24-bit/192 kHz ou mais), sem transcodificação.
 */

interface SSong {
  id: string;
  title: string;
  album?: string;
  albumId?: string;
  artist?: string;
  artistId?: string;
  track?: number;
  discNumber?: number;
  year?: number;
  genre?: string;
  genres?: Array<{ name: string }>;
  coverArt?: string;
  duration?: number;
  bitRate?: number;
  suffix?: string;
  samplingRate?: number;
  bitDepth?: number;
  channelCount?: number;
  replayGain?: { trackGain?: number; albumGain?: number; trackPeak?: number };
  playCount?: number;
}

interface SAlbum {
  id: string;
  name: string;
  artist?: string;
  artistId?: string;
  year?: number;
  genre?: string;
  coverArt?: string;
  songCount?: number;
  song?: SSong[];
  releaseTypes?: string[];
}

interface SArtist {
  id: string;
  name: string;
  coverArt?: string;
  artistImageUrl?: string;
  album?: SAlbum[];
}

type SResp<K extends string, T> = { 'subsonic-response': { status: 'ok' | 'failed'; error?: { message: string } } & Partial<Record<K, T>> };

const P = 'subsonic';
const id = (raw: string) => `${P}:${raw}`;
const CODECS: Record<string, Codec> = { flac: 'flac', m4a: 'alac', alac: 'alac', wav: 'wav', opus: 'opus', ogg: 'opus', aac: 'aac', mp3: 'mp3' };

export class SubsonicProvider implements CatalogProvider {
  readonly id = P;

  constructor(
    private readonly baseUrl: string,
    private readonly user: string,
    private readonly password: string,
  ) {}

  private url(method: string, params: Record<string, string | number | undefined> = {}): string {
    const salt = randomBytes(6).toString('hex');
    const u = new URL(`${this.baseUrl.replace(/\/$/, '')}/rest/${method}.view`);
    const all = { u: this.user, t: createHash('md5').update(this.password + salt).digest('hex'), s: salt, v: '1.16.1', c: 'sonora', f: 'json', ...params };
    for (const [k, v] of Object.entries(all)) if (v !== undefined) u.searchParams.set(k, String(v));
    return u.toString();
  }

  private async call<K extends string, T>(method: string, key: K, params?: Record<string, string | number | undefined>): Promise<T | undefined> {
    const r = (await getJson<SResp<K, T>>(P, this.url(method, params)))['subsonic-response'];
    if (r.status !== 'ok') throw new ProviderError(P, r.error?.message ?? 'erro');
    return r[key];
  }

  private cover(coverId?: string) {
    return coverId ? this.url('getCoverArt', { id: coverId, size: 1200 }) : undefined;
  }

  private mapSong = (s: SSong): Track => {
    const codec = CODECS[s.suffix ?? ''] ?? 'flac';
    const original: AudioSource = {
      url: this.url('stream', { id: s.id, format: 'raw' }),
      codec,
      bitrate: s.bitRate ?? 0,
      sampleRateHz: s.samplingRate ?? 44_100,
      bitDepth: s.bitDepth,
      channels: s.channelCount ?? 2,
      trackGainDb: s.replayGain?.trackGain,
      albumGainDb: s.replayGain?.albumGain,
      trackPeak: s.replayGain?.trackPeak,
    };
    // Versões transcodificadas para redes móveis / economia de dados.
    const lossy: AudioSource[] = [320, 128].map((br) => ({ ...original, url: this.url('stream', { id: s.id, format: 'opus', maxBitRate: br }), codec: 'opus', bitrate: br, sampleRateHz: 48_000, bitDepth: undefined }));
    return {
      id: id(s.id),
      provider: P,
      title: s.title,
      artists: [{ id: id(s.artistId ?? s.artist ?? 'unknown'), name: s.artist ?? 'Desconhecido' }],
      albumId: id(s.albumId ?? ''),
      albumTitle: s.album ?? '',
      artworkUrl: this.cover(s.coverArt),
      durationMs: (s.duration ?? 0) * 1000,
      trackNumber: s.track,
      discNumber: s.discNumber,
      year: s.year,
      genres: s.genres?.map((g) => g.name.toLowerCase()) ?? (s.genre ? [s.genre.toLowerCase()] : []),
      sources: [original, ...lossy],
    };
  };

  private mapAlbum = (a: SAlbum): Album => {
    const songs = a.song ?? [];
    const best = songs.map(this.mapSong).flatMap((t) => t.sources.slice(0, 1)).sort((x, y) => (y.bitDepth ?? 16) * y.sampleRateHz - (x.bitDepth ?? 16) * x.sampleRateHz)[0];
    const rt = a.releaseTypes?.[0]?.toLowerCase();
    return {
      id: id(a.id),
      provider: P,
      title: a.name,
      artists: [{ id: id(a.artistId ?? ''), name: a.artist ?? '' }],
      type: rt === 'single' ? 'single' : rt === 'ep' ? 'ep' : rt === 'compilation' ? 'compilation' : rt === 'live' ? 'live' : (a.songCount ?? 99) <= 3 ? 'single' : 'album',
      year: a.year,
      artworkUrl: this.cover(a.coverArt),
      genres: a.genre ? [a.genre.toLowerCase()] : [],
      trackIds: songs.map((s) => id(s.id)),
      bestQuality: best && { codec: best.codec, sampleRateHz: best.sampleRateHz, bitDepth: best.bitDepth },
    };
  };

  async search(q: SearchQuery, limit: number): Promise<SearchResults> {
    const query = [q.text, q.artist, q.album].filter(Boolean).join(' ');
    const r = await this.call<'searchResult3', { song?: SSong[]; album?: SAlbum[]; artist?: SArtist[] }>('search3', 'searchResult3', {
      query,
      songCount: limit,
      albumCount: Math.min(limit, 20),
      artistCount: Math.min(limit, 20),
    });
    return {
      tracks: (r?.song ?? []).map(this.mapSong),
      albums: (r?.album ?? []).map(this.mapAlbum),
      artists: (r?.artist ?? []).map((a) => ({ id: id(a.id), provider: P, name: a.name, imageUrl: a.artistImageUrl ?? this.cover(a.coverArt), genres: [] })),
    };
  }

  async track(rawId: string) {
    const s = await this.call<'song', SSong>('getSong', 'song', { id: rawId });
    return s ? this.mapSong(s) : null;
  }

  async album(rawId: string) {
    const a = await this.call<'album', SAlbum>('getAlbum', 'album', { id: rawId });
    return a ? { album: this.mapAlbum(a), tracks: (a.song ?? []).map(this.mapSong) } : null;
  }

  async artist(rawId: string): Promise<ArtistPage | null> {
    const a = await this.call<'artist', SArtist>('getArtist', 'artist', { id: rawId });
    if (!a) return null;
    const [info, top] = await Promise.all([
      this.call<'artistInfo2', { biography?: string; similarArtist?: SArtist[] }>('getArtistInfo2', 'artistInfo2', { id: rawId, count: 12 }).catch(() => undefined),
      this.call<'topSongs', { song?: SSong[] }>('getTopSongs', 'topSongs', { artist: a.name, count: 10 }).catch(() => undefined),
    ]);
    const albums = (a.album ?? []).map(this.mapAlbum);
    const topTracks = (top?.song ?? []).map(this.mapSong);
    return {
      artist: { id: id(a.id), provider: P, name: a.name, imageUrl: a.artistImageUrl ?? this.cover(a.coverArt), genres: [...new Set(albums.flatMap((x) => x.genres))], bio: info?.biography },
      topTracks,
      albums: albums.filter((x) => x.type === 'album' || x.type === 'ep' || x.type === 'live'),
      singles: albums.filter((x) => x.type === 'single'),
      compilations: albums.filter((x) => x.type === 'compilation'),
      related: (info?.similarArtist ?? []).map((s) => ({ id: id(s.id), provider: P, name: s.name, imageUrl: s.artistImageUrl, genres: [] })),
    };
  }

  async tracksByGenre(genre: string, limit: number) {
    const r = await this.call<'songsByGenre', { song?: SSong[] }>('getSongsByGenre', 'songsByGenre', { genre, count: limit });
    return (r?.song ?? []).map(this.mapSong);
  }
}
