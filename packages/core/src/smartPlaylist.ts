import type { SmartRule, SmartRules, Track } from './types';
import { isHiRes, isLossless } from './quality';
import { mulberry32 } from './queue';

/** Dados do usuário por faixa necessários para avaliar regras. */
export interface TrackUserStats {
  playCount: number;
  liked: boolean;
  lastPlayedAt?: number;
}

export function matchesRule(t: Track, s: TrackUserStats, r: SmartRule, now: number): boolean {
  switch (r.field) {
    case 'genre': {
      const has = t.genres.some((g) => g.toLowerCase() === r.value.toLowerCase());
      return r.op === 'is' ? has : !has;
    }
    case 'year':
      return t.year !== undefined && t.year >= r.value[0] && t.year <= r.value[1];
    case 'artist': {
      const has = t.artists.some((a) => a.id === r.value);
      return r.op === 'is' ? has : !has;
    }
    case 'playCount':
      return r.op === 'gte' ? s.playCount >= r.value : s.playCount <= r.value;
    case 'liked':
      return s.liked === r.value;
    case 'lastPlayedDays': {
      if (s.lastPlayedAt === undefined) return r.op === 'gte'; // nunca tocada = "há muito tempo"
      const days = (now - s.lastPlayedAt) / 86_400_000;
      return r.op === 'gte' ? days >= r.value : days <= r.value;
    }
    case 'quality':
      if (r.value === 'spatial') return t.sources.some((x) => x.spatial);
      if (r.value === 'hires') return t.sources.some(isHiRes);
      return t.sources.some(isLossless);
  }
}

/** Avalia uma playlist inteligente sobre a biblioteca; reavaliada a cada abertura/sync. */
export function evaluateSmartPlaylist(
  library: Track[],
  stats: (id: string) => TrackUserStats,
  rules: SmartRules,
  now = Date.now(),
  seed = 1,
): Track[] {
  const test = (t: Track) => {
    const s = stats(t.id);
    const results = rules.rules.map((r) => matchesRule(t, s, r, now));
    return rules.match === 'all' ? results.every(Boolean) : results.some(Boolean);
  };
  let out = library.filter(test);
  switch (rules.sort) {
    case 'playCount':
      out.sort((a, b) => stats(b.id).playCount - stats(a.id).playCount);
      break;
    case 'recent':
      out.sort((a, b) => (stats(b.id).lastPlayedAt ?? 0) - (stats(a.id).lastPlayedAt ?? 0));
      break;
    case 'popularity':
      out.sort((a, b) => (b.popularity ?? 0) - (a.popularity ?? 0));
      break;
    case 'random': {
      const rand = mulberry32(seed);
      out = out.map((t) => ({ t, r: rand() })).sort((a, b) => a.r - b.r).map((x) => x.t);
      break;
    }
  }
  return rules.limit ? out.slice(0, rules.limit) : out;
}
