import { describe, expect, it } from 'vitest';
import {
  activeLineIndex,
  applyOps,
  compareEntries,
  evaluateSmartPlaylist,
  HybridClock,
  keyBetween,
  keysBetween,
  materialize,
  normalizationGain,
  parseLrc,
  parseSearch,
  pickSource,
  qualityLabel,
  smartShuffle,
  type ChangeOp,
} from '../src';
import { src, track } from './fixtures';

describe('lrc', () => {
  it('parseia linhas, timestamps múltiplos, offset e palavras', () => {
    const l = parseLrc('[ti:x]\n[offset:+100]\n[00:01.50][00:10.00]Refrão\n[00:05.00]<00:05.00>Oi <00:05.50>mundo');
    expect(l.synced).toBe(true);
    expect(l.lines.map((x) => x.timeMs)).toEqual([1400, 4900, 9900]);
    expect(l.lines[1]!.text).toBe('Oi mundo');
    expect(l.lines[1]!.words![1]!.timeMs).toBe(5400);
    expect(activeLineIndex(l.lines, 6000)).toBe(1);
    expect(activeLineIndex(l.lines, 0)).toBe(-1);
  });
  it('letra sem tempo vira não sincronizada', () => {
    expect(parseLrc('linha 1\nlinha 2')).toEqual({ synced: false, lines: [{ timeMs: 0, text: 'linha 1' }, { timeMs: 0, text: 'linha 2' }] });
  });
});

describe('fractional index', () => {
  it('gera chaves ordenadas entre vizinhos', () => {
    let keys = keysBetween(null, null, 50);
    for (let i = 1; i < keys.length; i++) expect(keys[i]! > keys[i - 1]!).toBe(true);
    // insere repetidamente no mesmo ponto
    let a = keys[0]!;
    const b = keys[1]!;
    for (let i = 0; i < 200; i++) {
      const k = keyBetween(a, b);
      expect(k > a && k < b).toBe(true);
      a = k;
    }
    keys = [keyBetween(null, keys[0]!), ...keys];
    expect(keys[0]! < keys[1]!).toBe(true);
    expect(compareEntries({ position: 'a', entryId: '2' }, { position: 'a', entryId: '1' })).toBe(1);
  });
});

describe('sync', () => {
  it('LWW converge independente da ordem', () => {
    let t = 1000;
    const c1 = new HybridClock('d1', () => t);
    const c2 = new HybridClock('d2', () => t);
    const ops: ChangeOp[] = [
      { entity: 'playlist', entityId: 'p', field: 'name', value: 'A', hlc: c1.tick(), deviceId: 'd1' },
      { entity: 'playlist', entityId: 'p', field: 'name', value: 'B', hlc: c2.tick(), deviceId: 'd2' },
      { entity: 'like', entityId: 't1', field: '$exists', value: true, hlc: c1.tick(), deviceId: 'd1' },
    ];
    t = 2000;
    ops.push({ entity: 'like', entityId: 't1', field: '$exists', value: false, hlc: c2.tick(), deviceId: 'd2' });
    const s1 = applyOps(new Map(), ops);
    const s2 = applyOps(new Map(), [...ops].reverse());
    expect(materialize(s1, 'playlist')).toEqual(materialize(s2, 'playlist'));
    expect(materialize(s1, 'playlist')[0]!.name).toBe('B'); // mesmo wall, d2 > d1
    expect(materialize(s1, 'like')).toEqual([]);
  });
  it('receive avança o relógio além do remoto', () => {
    const c = new HybridClock('a', () => 5);
    c.receive(new HybridClock('b', () => 100).tick());
    expect(c.tick() > '00000002s').toBe(true);
  });
});

describe('search', () => {
  it('extrai filtros avançados', () => {
    const q = parseSearch('artist:"Milton Nascimento" year:1970-1979 genre:mpb -genre:funk popularity:>70 travessia quality:hires');
    expect(q).toMatchObject({ artist: 'Milton Nascimento', yearFrom: 1970, yearTo: 1979, genres: ['mpb'], excludeGenres: ['funk'], minPopularity: 70, quality: 'hires', text: 'travessia' });
  });
});

describe('quality', () => {
  const caps = { maxSampleRateHz: 192_000, maxBitDepth: 24, supportsFlac: true, supportsAlac: true, supportsSpatial: false };
  const sources = [src({ codec: 'aac', bitrate: 256, bitDepth: undefined }), src({}), src({ bitDepth: 24, sampleRateHz: 192_000, bitrate: 9000 }), src({ codec: 'eac3-joc', spatial: true, bitrate: 768 })];
  it('escolhe a maior fidelidade permitida', () => {
    expect(qualityLabel(pickSource(sources, 'max', caps)!)).toBe('24-bit/192 kHz FLAC');
    expect(qualityLabel(pickSource(sources, 'lossless', caps)!)).toBe('16-bit/44.1 kHz FLAC');
    expect(qualityLabel(pickSource(sources, 'high', caps)!)).toBe('AAC 256');
    expect(pickSource(sources, 'max', { ...caps, maxSampleRateHz: 96_000 })!.sampleRateHz).toBe(44_100);
    expect(pickSource(sources, 'max', { ...caps, supportsSpatial: true }, true)!.spatial).toBe(true);
  });
  it('normalização nunca clipa', () => {
    expect(normalizationGain(src({ trackGainDb: 6, trackPeak: 0.9 }), 'track', -18)).toBeCloseTo(1 / 0.9);
    expect(normalizationGain(src({ trackGainDb: -6 }), 'track', -18)).toBeCloseTo(0.501, 2);
    expect(normalizationGain(src({ trackGainDb: -6 }), 'off')).toBe(1);
  });
});

describe('smart playlists e shuffle', () => {
  it('avalia regras', () => {
    const lib = [track('1', 'a', { year: 1975, genres: ['mpb'] }), track('2', 'b', { year: 1990, genres: ['mpb'] }), track('3', 'c', { year: 1976, genres: ['rock'] })];
    const out = evaluateSmartPlaylist(lib, () => ({ playCount: 0, liked: true }), {
      match: 'all',
      rules: [
        { field: 'genre', op: 'is', value: 'MPB' },
        { field: 'year', op: 'between', value: [1970, 1979] },
      ],
    });
    expect(out.map((t) => t.id)).toEqual(['1']);
  });
  it('smart shuffle evita o mesmo artista em sequência quando possível', () => {
    const ts = [...'aaaabbbbcccc'].map((a, i) => track(String(i), a));
    const out = smartShuffle(ts, 42);
    expect(out).toHaveLength(12);
    let adjacent = 0;
    for (let i = 1; i < out.length; i++) if (out[i]!.artists[0]!.id === out[i - 1]!.artists[0]!.id) adjacent++;
    expect(adjacent).toBeLessThanOrEqual(2);
  });
});
