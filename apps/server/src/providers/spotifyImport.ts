import type { Track } from '@sonora/core';
import type { Repos } from '../repos/types';
import type { CatalogProvider } from './types';

/**
 * Resolve IDs `spotify:<id>` para exibição: faixas de playlists importadas que
 * ainda não têm fonte de áudio legal. Não tem `sources` — o app mostra a faixa
 * e oferece abrir no Spotify.
 */
export class SpotifyImportProvider implements CatalogProvider {
  readonly id = 'spotify';

  constructor(private readonly repos: Repos) {}

  async search() {
    return { tracks: [], albums: [], artists: [] };
  }

  async track(raw: string): Promise<Track | null> {
    const it = await this.repos.spotify.getTrack(raw);
    if (!it) return null;
    return {
      id: `spotify:${it.spotifyId}`,
      provider: 'spotify',
      title: it.title,
      artists: it.artists.map((name) => ({ id: `spotify-artist:${name}`, name })),
      albumId: '',
      albumTitle: it.album,
      artworkUrl: it.artworkUrl,
      durationMs: it.durationMs,
      genres: [],
      isrc: it.isrc,
      sources: [],
    };
  }

  async album() {
    return null;
  }

  async artist() {
    return null;
  }

  async tracksByGenre() {
    return [];
  }
}
