import { describe, expect, it } from 'vitest';
import { buildProfile, cooccurrence, rankCandidates, sessionize } from '../src';
import { ev, track } from './fixtures';

const NOW = Date.UTC(2026, 5, 1);

describe('recommend', () => {
  it('sessões e co-ocorrência', () => {
    const events = [ev({ startedAt: 0, trackId: 'a' }), ev({ startedAt: 200_000, trackId: 'b' }), ev({ startedAt: 10_000_000, trackId: 'c' })];
    const s = sessionize(events);
    expect(s).toEqual([['a', 'b'], ['c']]);
    expect(cooccurrence(s).get('a')!.get('b')).toBe(1);
  });

  it('perfil penaliza skips e o ranking diversifica artistas', () => {
    const events = [
      ev({ startedAt: NOW - 1000, trackId: 'liked', artistIds: ['A'] }),
      ...Array.from({ length: 5 }, (_, i) => ev({ startedAt: NOW - i, trackId: 'hated', artistIds: ['Z'], skipped: true, listenedMs: 5000 })),
    ];
    const p = buildProfile(events, NOW);
    expect(p.artists.get('A')).toBeGreaterThan(0);
    expect(p.tracks.get('hated')).toBeLessThan(0);

    const cands = [
      ...['1', '2', '3', '4'].map((id) => ({ track: track(id, 'A'), cooc: 1 })),
      { track: track('5', 'B'), cooc: 0.8 },
      { track: track('hated', 'Z'), cooc: 1 },
    ];
    const out = rankCandidates(cands, p, { discovery: 0.7, limit: 10, maxPerArtist: 2 });
    expect(out.map((t) => t.id)).not.toContain('hated');
    expect(out.filter((t) => t.artists[0]!.id === 'A')).toHaveLength(2);
    for (let i = 1; i < out.length; i++) expect(out[i]!.artists[0]!.id).not.toBe(out[i - 1]!.artists[0]!.id);
  });
});
