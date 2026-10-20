import type { Album, Artist, ArtistPage, AudioSource, SearchQuery, Track } from '@sonora/core';

export interface SearchResults {
  tracks: Track[];
  albums: Album[];
  artists: Artist[];
}

/**
 * Fonte de catálogo licenciada. IDs devolvidos já vêm com o prefixo do
 * provider (`jamendo:123`) para que o CatalogService saiba rotear.
 *
 * Integrações possíveis: Jamendo (CC), servidores Subsonic/Navidrome (acervo
 * próprio do usuário), e APIs B2B de distribuidoras/agregadoras licenciadas
 * mediante contrato (ver docs/ARCHITECTURE.md §Catálogo).
 */
export interface CatalogProvider {
  readonly id: string;
  search(q: SearchQuery, limit: number): Promise<SearchResults>;
  track(rawId: string): Promise<Track | null>;
  album(rawId: string): Promise<{ album: Album; tracks: Track[] } | null>;
  artist(rawId: string): Promise<ArtistPage | null>;
  /** Faixas por gênero ordenadas por popularidade (candidatos de recomendação). */
  tracksByGenre(genre: string, limit: number): Promise<Track[]>;
  /**
   * Catálogo licenciado: URLs assinadas de curta duração, emitidas só depois que
   * o servidor validou assinatura e território. Providers sem isso usam `track.sources`.
   */
  streamSources?(rawId: string, ctx: StreamContext): Promise<AudioSource[]>;
}

export interface StreamContext {
  userId: string;
  deviceId: string;
  country: string;
  purpose: 'stream' | 'download';
}

export class ProviderError extends Error {
  constructor(
    readonly provider: string,
    message: string,
    readonly status?: number,
  ) {
    super(`[${provider}] ${message}`);
  }
}

export async function getJson<T>(provider: string, url: string, timeoutMs = 8000): Promise<T> {
  const res = await fetch(url, { signal: AbortSignal.timeout(timeoutMs), headers: { accept: 'application/json' } });
  if (!res.ok) throw new ProviderError(provider, `HTTP ${res.status}`, res.status);
  return (await res.json()) as T;
}
