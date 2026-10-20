import type { ID, PlayEvent, Track } from './types';
import { mulberry32 } from './queue';

/**
 * Núcleo de recomendação (funções puras, usadas pelo servidor).
 *
 * Pipeline em 2 estágios, como sistemas de produção:
 *   1. Geração de candidatos: co-escuta (item-item a partir de sessões de todos
 *      os usuários), mesmos artistas/relacionados, mesmos gêneros e, no servidor,
 *      vizinhos por embedding (pgvector) para cold start.
 *   2. Ranking: perfil de gosto do usuário com decaimento temporal +
 *      penalidade de skip + novidade, seguido de re-ranking por diversidade.
 */

export interface TasteProfile {
  artists: Map<ID, number>;
  genres: Map<string, number>;
  tracks: Map<ID, number>;
  /** Faixas ouvidas recentemente (evita repetir em mixes de descoberta). */
  recent: Set<ID>;
}

const HALF_LIFE_DAYS = 30;

/** Afinidade com decaimento exponencial; skips contam negativamente. */
export function buildProfile(events: PlayEvent[], now: number): TasteProfile {
  const p: TasteProfile = { artists: new Map(), genres: new Map(), tracks: new Map(), recent: new Set() };
  const lambda = Math.LN2 / (HALF_LIFE_DAYS * 86_400_000);
  for (const e of events) {
    const decay = Math.exp(-lambda * Math.max(0, now - e.startedAt));
    const completion = e.trackDurationMs > 0 ? Math.min(1, e.listenedMs / e.trackDurationMs) : 0;
    const w = decay * (e.skipped ? -0.5 : 0.25 + completion);
    bump(p.tracks, e.trackId, w);
    for (const a of e.artistIds) bump(p.artists, a, w);
    for (const g of e.genres) bump(p.genres, g, w);
    if (now - e.startedAt < 14 * 86_400_000) p.recent.add(e.trackId);
  }
  normalizeMap(p.artists);
  normalizeMap(p.genres);
  normalizeMap(p.tracks);
  return p;
}

function bump<K>(m: Map<K, number>, k: K, v: number) {
  m.set(k, (m.get(k) ?? 0) + v);
}

function normalizeMap<K>(m: Map<K, number>) {
  let max = 0;
  for (const v of m.values()) max = Math.max(max, Math.abs(v));
  if (max > 0) for (const [k, v] of m) m.set(k, v / max);
}

/** Divide o histórico em sessões (pausa > 30 min encerra a sessão). */
export function sessionize(events: PlayEvent[], gapMs = 30 * 60_000): ID[][] {
  const sorted = [...events].sort((a, b) => a.startedAt - b.startedAt);
  const sessions: ID[][] = [];
  let cur: ID[] = [];
  let lastEnd = -Infinity;
  for (const e of sorted) {
    if (e.startedAt - lastEnd > gapMs && cur.length) {
      sessions.push(cur);
      cur = [];
    }
    if (!e.skipped) cur.push(e.trackId);
    lastEnd = e.startedAt + e.listenedMs;
  }
  if (cur.length) sessions.push(cur);
  return sessions;
}

/**
 * Co-ocorrência item-item dentro de uma janela deslizante na sessão.
 * Retorna contagens brutas; o servidor normaliza (PMI/cosine) e persiste top-K por faixa.
 */
export function cooccurrence(sessions: ID[][], window = 5): Map<ID, Map<ID, number>> {
  const m = new Map<ID, Map<ID, number>>();
  for (const s of sessions) {
    for (let i = 0; i < s.length; i++) {
      for (let j = i + 1; j < Math.min(s.length, i + 1 + window); j++) {
        const a = s[i]!;
        const b = s[j]!;
        if (a === b) continue;
        const w = 1 / (j - i);
        addPair(m, a, b, w);
        addPair(m, b, a, w);
      }
    }
  }
  return m;
}

function addPair(m: Map<ID, Map<ID, number>>, a: ID, b: ID, w: number) {
  let row = m.get(a);
  if (!row) m.set(a, (row = new Map()));
  row.set(b, (row.get(b) ?? 0) + w);
}

export interface Candidate {
  track: Track;
  /** Sinal de co-escuta já normalizado (0..1). */
  cooc?: number;
  /** Similaridade por embedding (0..1). */
  embedding?: number;
}

export interface RankOptions {
  /** 0 = só familiar, 1 = só novidade. */
  discovery: number;
  excludeRecent?: boolean;
  /** Máximo de faixas do mesmo artista no resultado. */
  maxPerArtist?: number;
  limit: number;
}

export function scoreCandidate(c: Candidate, p: TasteProfile, discovery: number): number {
  const t = c.track;
  const artist = Math.max(0, ...t.artists.map((a) => p.artists.get(a.id) ?? 0));
  const genre = t.genres.length ? t.genres.reduce((s, g) => s + (p.genres.get(g) ?? 0), 0) / t.genres.length : 0;
  const familiarity = p.tracks.get(t.id) ?? 0;
  const known = familiarity > 0 ? 1 : 0;
  const pop = (t.popularity ?? 50) / 100;

  const relevance = 0.35 * (c.cooc ?? 0) + 0.2 * (c.embedding ?? 0) + 0.2 * artist + 0.15 * genre + 0.1 * pop;
  // Descoberta favorece o que o usuário nunca ouviu; modo familiar favorece favoritas.
  const noveltyTerm = discovery * (1 - known) + (1 - discovery) * familiarity;
  // Faixas com afinidade negativa (muito puladas) são enterradas.
  const penalty = familiarity < 0 ? 1 : 0;
  return relevance + 0.4 * noveltyTerm - penalty;
}

/** Ranking + re-ranking por diversidade (limite por artista e sem o mesmo artista em sequência). */
export function rankCandidates(cands: Candidate[], p: TasteProfile, opts: RankOptions): Track[] {
  const seen = new Set<ID>();
  const scored = cands
    .filter((c) => {
      if (seen.has(c.track.id)) return false;
      seen.add(c.track.id);
      return !(opts.excludeRecent && p.recent.has(c.track.id));
    })
    .map((c) => ({ t: c.track, s: scoreCandidate(c, p, opts.discovery) }))
    .filter((x) => x.s > 0)
    .sort((a, b) => b.s - a.s);

  const maxPer = opts.maxPerArtist ?? 2;
  const perArtist = new Map<ID, number>();
  const out: Track[] = [];
  const deferred: Track[] = [];
  for (const { t } of scored) {
    if (out.length >= opts.limit) break;
    const main = t.artists[0]?.id ?? '';
    if ((perArtist.get(main) ?? 0) >= maxPer) continue;
    const prevMain = out[out.length - 1]?.artists[0]?.id;
    if (prevMain === main) {
      deferred.push(t);
      continue;
    }
    out.push(t);
    perArtist.set(main, (perArtist.get(main) ?? 0) + 1);
    // Tenta encaixar algo adiado assim que o artista anterior mudar.
    const idx = deferred.findIndex((d) => d.artists[0]?.id !== main && (perArtist.get(d.artists[0]?.id ?? '') ?? 0) < maxPer);
    if (idx >= 0 && out.length < opts.limit) {
      const [d] = deferred.splice(idx, 1);
      out.push(d!);
      bump(perArtist, d!.artists[0]?.id ?? '', 1);
    }
  }
  return out;
}

/**
 * Daily Mixes: agrupa as faixas favoritas por gênero dominante e gera até `n`
 * mixes, cada um ~70% familiar e ~30% novo (o servidor fornece os candidatos novos por gênero).
 */
export function dailyMixClusters(favorites: Track[], p: TasteProfile, n = 6): Array<{ genre: string; seeds: Track[] }> {
  const byGenre = new Map<string, Track[]>();
  for (const t of favorites) {
    const g = [...t.genres].sort((a, b) => (p.genres.get(b) ?? 0) - (p.genres.get(a) ?? 0))[0] ?? 'variados';
    const list = byGenre.get(g) ?? [];
    list.push(t);
    byGenre.set(g, list);
  }
  return [...byGenre.entries()]
    .filter(([, ts]) => ts.length >= 3)
    .sort((a, b) => (p.genres.get(b[0]) ?? 0) - (p.genres.get(a[0]) ?? 0))
    .slice(0, n)
    .map(([genre, seeds]) => ({ genre, seeds }));
}

/** Embaralhamento "inteligente": espalha faixas do mesmo artista ao longo da lista. */
export function smartShuffle<T extends { artists: { id: ID }[] }>(tracks: T[], seed: number): T[] {
  const rand = mulberry32(seed);
  const groups = new Map<ID, T[]>();
  for (const t of tracks) {
    const a = t.artists[0]?.id ?? '';
    const g = groups.get(a) ?? [];
    g.push(t);
    groups.set(a, g);
  }
  // Cada faixa recebe uma posição alvo uniforme dentro do seu grupo + jitter.
  const placed: Array<{ t: T; pos: number }> = [];
  for (const g of groups.values()) {
    const offset = rand() / g.length;
    const shuffledGroup = g.map((t) => ({ t, r: rand() })).sort((a, b) => a.r - b.r);
    shuffledGroup.forEach(({ t }, i) => placed.push({ t, pos: offset + i / g.length + (rand() - 0.5) * 0.1 / g.length }));
  }
  return placed.sort((a, b) => a.pos - b.pos).map((x) => x.t);
}
