import Constants from 'expo-constants';
import * as SecureStore from 'expo-secure-store';
import type { Album, ArtistPage, AudioSource, ChangeOp, Entitlement, ListeningSummary, PlayEvent, RankedItem, SearchQuery, Track } from '@sonora/core';

const BASE: string = (Constants.expoConfig?.extra?.apiUrl as string | undefined) ?? 'http://localhost:8080';
const TOKEN_KEY = 'sonora.token';

let token: string | null = null;

export async function loadToken(): Promise<string | null> {
  token = await SecureStore.getItemAsync(TOKEN_KEY);
  return token;
}

export async function setToken(t: string | null) {
  token = t;
  if (t) await SecureStore.setItemAsync(TOKEN_KEY, t);
  else await SecureStore.deleteItemAsync(TOKEN_KEY);
}

export class ApiError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

async function req<T>(path: string, init: RequestInit = {}): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    ...init,
    headers: {
      'content-type': 'application/json',
      ...(token ? { authorization: `Bearer ${token}` } : {}),
      ...init.headers,
    },
  });
  if (!res.ok) {
    const body = (await res.json().catch(() => ({}))) as { error?: string };
    throw new ApiError(res.status, body.error ?? `HTTP ${res.status}`);
  }
  return (await res.json()) as T;
}

const post = <T>(path: string, body: unknown) => req<T>(path, { method: 'POST', body: JSON.stringify(body) });
const qs = (o: Record<string, string | number | undefined>) =>
  Object.entries(o)
    .filter(([, v]) => v !== undefined)
    .map(([k, v]) => `${k}=${encodeURIComponent(String(v))}`)
    .join('&');

export interface Mix {
  id: string;
  title: string;
  subtitle: string;
  tracks: Track[];
}

export interface StoredOp extends ChangeOp {
  seq: number;
  scope: string;
  authorId: string;
}

export const api = {
  register: (email: string, password: string, displayName?: string) => post<{ token: string; user: { id: string; displayName: string } }>('/auth/register', { email, password, displayName }),
  login: (email: string, password: string) => post<{ token: string; user: { id: string; displayName: string } }>('/auth/login', { email, password }),
  me: () => req<{ id: string; email: string; displayName: string; entitlement: Entitlement }>('/me'),
  stream: (id: string, deviceId: string, purpose: 'stream' | 'download' = 'stream') =>
    req<{ sources: AudioSource[]; licenseExpiresAt?: number }>(`/tracks/${encodeURIComponent(id)}/stream?${qs({ deviceId, purpose })}`),
  renewDownloads: (ids: string[]) => post<{ renewed: string[]; revoked: string[]; expiresAt: number }>('/downloads/renew', { ids }),

  search: (q: string, limit = 30) => req<{ query: SearchQuery; tracks: Track[]; albums: Album[]; artists: ArtistPage['artist'][]; errors: string[] }>(`/search?${qs({ q, limit })}`),
  track: (id: string) => req<Track>(`/tracks/${encodeURIComponent(id)}`),
  tracks: (ids: string[]) => post<{ tracks: Track[] }>('/tracks/batch', { ids }),
  album: (id: string) => req<{ album: Album; tracks: Track[] }>(`/albums/${encodeURIComponent(id)}`),
  artist: (id: string) => req<ArtistPage>(`/artists/${encodeURIComponent(id)}`),
  lyrics: (id: string) => req<{ synced: string | null; plain: string | null }>(`/tracks/${encodeURIComponent(id)}/lyrics`),

  scopes: () => req<{ scopes: string[] }>('/sync/scopes'),
  push: (scope: string, ops: ChangeOp[]) => post<{ cursor: number }>('/sync/push', { scope, ops }),
  pull: (scope: string, cursor: number) => req<{ ops: StoredOp[]; cursor: number; hasMore: boolean }>(`/sync/pull?${qs({ scope, cursor })}`),
  claimPlaylist: (id: string) => post<{ role: string }>(`/playlists/${encodeURIComponent(id)}/claim`, {}),
  invite: (id: string) => post<{ token: string; url: string }>(`/playlists/${encodeURIComponent(id)}/invites`, {}),
  acceptInvite: (t: string) => post<{ playlistId: string; scope: string }>(`/invites/${encodeURIComponent(t)}/accept`, {}),

  uploadPlays: (events: PlayEvent[]) => post<{ accepted: number }>('/plays', { events }),
  summary: (from: number, to: number, tz: number) =>
    req<ListeningSummary & { tracks: Record<string, Track>; artistNames: Record<string, string> }>(`/stats/summary?${qs({ from, to, tz })}`),
  top: (kind: 'tracks' | 'artists' | 'genres', from?: number, to?: number) => req<{ items: RankedItem[] }>(`/stats/top?${qs({ kind, from, to })}`),

  home: () => req<{ sections: { id: string; title: string; tracks?: Track[]; mixes?: Mix[] }[] }>('/recs/home'),
  radio: (seed: string, exclude: string[] = []) => req<{ tracks: Track[] }>(`/recs/radio?${qs({ seed, exclude: exclude.slice(-100).join(',') || undefined })}`),
  playlistRecs: (trackIds: string[]) => post<{ tracks: Track[] }>('/recs/playlist', { trackIds }),
};
