import type { SpotifyItem } from '@sonora/core';

/**
 * Cliente mínimo da Web API do Spotify, só para LER playlists (metadados).
 * Nenhum áudio é obtido do Spotify.
 *
 * Regras atuais da API (fev/2026): itens de playlist vêm de
 * `GET /playlists/{id}/items` e só para playlists que o usuário criou ou
 * colabora; apps em Development Mode atendem até 5 usuários e o dono do
 * Client ID precisa ser Premium.
 */

export type FetchFn = (url: string, init?: RequestInit) => Promise<Response>;

export class SpotifyError extends Error {
  constructor(
    readonly status: number,
    message: string,
  ) {
    super(message);
  }
}

export interface TokenSet {
  accessToken: string;
  refreshToken: string;
  expiresAt: number;
}

export interface PlaylistMeta {
  id: string;
  name: string;
  snapshotId: string;
  artworkUrl?: string;
  ownerName: string;
}

interface RawTrack {
  id: string | null;
  type?: string;
  is_local?: boolean;
  name: string;
  duration_ms: number;
  artists: Array<{ name: string }>;
  album?: { name: string; images?: Array<{ url: string }> };
  external_ids?: { isrc?: string };
}

interface RawEntry {
  added_at?: string;
  /** Formato novo (fev/2026). */
  item?: RawTrack | null;
  /** Formato antigo. */
  track?: RawTrack | null;
}

export class SpotifyClient {
  constructor(
    readonly clientId: string,
    private readonly fetchFn: FetchFn = fetch,
    private readonly accountsBase = 'https://accounts.spotify.com',
    private readonly apiBase = 'https://api.spotify.com/v1',
  ) {}

  private async token(params: Record<string, string>): Promise<TokenSet & { refreshToken: string }> {
    const res = await this.fetchFn(`${this.accountsBase}/api/token`, {
      method: 'POST',
      headers: { 'content-type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({ client_id: this.clientId, ...params }).toString(),
    });
    const body = (await res.json().catch(() => ({}))) as { access_token?: string; refresh_token?: string; expires_in?: number; error_description?: string };
    if (!res.ok || !body.access_token) throw new SpotifyError(res.status, body.error_description ?? 'falha ao autenticar no Spotify');
    return { accessToken: body.access_token, refreshToken: body.refresh_token ?? params.refresh_token ?? '', expiresAt: Date.now() + (body.expires_in ?? 3600) * 1000 - 60_000 };
  }

  /** Troca o código do OAuth (fluxo PKCE, sem client secret) por tokens. */
  exchangeCode(code: string, codeVerifier: string, redirectUri: string) {
    return this.token({ grant_type: 'authorization_code', code, code_verifier: codeVerifier, redirect_uri: redirectUri });
  }

  /** Renova o access token; o Spotify pode devolver um refresh token novo. */
  refresh(refreshToken: string) {
    return this.token({ grant_type: 'refresh_token', refresh_token: refreshToken });
  }

  private async get<T>(path: string, accessToken: string, attempt = 0): Promise<T> {
    const res = await this.fetchFn(`${this.apiBase}${path}`, { headers: { authorization: `Bearer ${accessToken}` } });
    if (res.status === 429 && attempt < 3) {
      const wait = Number(res.headers.get('retry-after') ?? 1) * 1000;
      await new Promise((r) => setTimeout(r, Math.min(wait, 30_000)));
      return this.get(path, accessToken, attempt + 1);
    }
    if (!res.ok) {
      const body = (await res.json().catch(() => ({}))) as { error?: { message?: string } };
      throw new SpotifyError(res.status, body.error?.message ?? `HTTP ${res.status}`);
    }
    return (await res.json()) as T;
  }

  me(accessToken: string) {
    return this.get<{ id: string; display_name?: string }>('/me', accessToken);
  }

  async playlist(id: string, accessToken: string): Promise<PlaylistMeta> {
    const r = await this.get<{ id: string; name: string; snapshot_id: string; images?: Array<{ url: string }>; owner?: { display_name?: string; id: string } }>(
      `/playlists/${id}?fields=id,name,snapshot_id,images,owner(id,display_name)`,
      accessToken,
    );
    return { id: r.id, name: r.name, snapshotId: r.snapshot_id, artworkUrl: r.images?.[0]?.url, ownerName: r.owner?.display_name ?? r.owner?.id ?? '' };
  }

  /** Todas as faixas da playlist, na ordem. Ignora episódios de podcast e arquivos locais. */
  async items(id: string, accessToken: string): Promise<SpotifyItem[]> {
    const out: SpotifyItem[] = [];
    for (let offset = 0; ; offset += 50) {
      const page = await this.get<{ items: RawEntry[]; next: string | null }>(`/playlists/${id}/items?limit=50&offset=${offset}`, accessToken);
      for (const e of page.items) {
        const t = e.item ?? e.track;
        if (!t || !t.id || t.is_local || (t.type && t.type !== 'track')) continue;
        out.push({
          spotifyId: t.id,
          title: t.name,
          artists: t.artists.map((a) => a.name),
          album: t.album?.name ?? '',
          durationMs: t.duration_ms,
          isrc: t.external_ids?.isrc,
          artworkUrl: t.album?.images?.[0]?.url,
          addedAt: e.added_at ? Date.parse(e.added_at) : 0,
        });
      }
      if (!page.next) break;
    }
    return out;
  }
}
