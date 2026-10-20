import { describe, expect, it } from 'vitest';
import { bestMatch, cleanTitle, diffPlaylist, matchScore, parseSpotifyPlaylist, type SpotifyItem } from '../src';
import { track } from './fixtures';

const ID = '37i9dQZF1DXcBWIGoYBM5M';
const item = (id: string, p: Partial<SpotifyItem> = {}): SpotifyItem => ({ spotifyId: id, title: 'Águas de Março', artists: ['Elis Regina', 'Tom Jobim'], album: 'Elis & Tom', durationMs: 211_000, addedAt: 0, ...p });

describe('spotify', () => {
  it('extrai o ID de links, URIs e IDs puros', () => {
    expect(parseSpotifyPlaylist(`https://open.spotify.com/playlist/${ID}?si=abc123`)).toBe(ID);
    expect(parseSpotifyPlaylist(`https://open.spotify.com/intl-pt/playlist/${ID}`)).toBe(ID);
    expect(parseSpotifyPlaylist(`spotify:playlist:${ID}`)).toBe(ID);
    expect(parseSpotifyPlaylist(` ${ID} `)).toBe(ID);
    expect(parseSpotifyPlaylist(`https://open.spotify.com/album/${ID}`)).toBeNull();
    expect(parseSpotifyPlaylist(`https://evil.com/playlist/${ID}`)).toBeNull();
    expect(parseSpotifyPlaylist('qualquer coisa')).toBeNull();
  });

  it('detecta adições, remoções e reordenação, inclusive faixas repetidas', () => {
    const a = item('a'), b = item('b'), c = item('c');
    expect(diffPlaylist([a, b], [a, b, c])).toEqual({ added: [c], removed: [], reordered: false });
    expect(diffPlaylist([a, b, c], [a, c])).toEqual({ added: [], removed: ['b#0'], reordered: false });
    expect(diffPlaylist([a, b], [b, a]).reordered).toBe(true);
    expect(diffPlaylist([a], [a, a]).added).toEqual([a]);
  });

  it('casa por ISRC ou por título/artista/duração e recusa casamentos fracos', () => {
    expect(cleanTitle('Something - Remastered 2009')).toBe('something');
    expect(cleanTitle('Garota de Ipanema (feat. Astrud Gilberto)')).toBe('garota de ipanema');
    const good = track('x', 'elis', { title: 'Aguas De Marco', artists: [{ id: 'e', name: 'Elis Regina' }], durationMs: 212_000 });
    const wrongArtist = track('y', 'z', { title: 'Águas de Março', artists: [{ id: 'z', name: 'Outro Cantor' }], durationMs: 180_000 });
    expect(matchScore(item('s'), good)).toBe(1);
    expect(bestMatch(item('s'), [wrongArtist])).toBeNull();
    expect(bestMatch(item('s'), [wrongArtist, good])!.track.id).toBe('x');
    expect(matchScore(item('s', { isrc: 'BRAAA0000001' }), { ...good, isrc: 'BRBBB0000002' })).toBe(0);
  });
});
