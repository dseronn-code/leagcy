import type { ID, PlayEvent } from './types';

/**
 * Estatísticas de escuta calculadas a partir de PlayEvents.
 *
 * O mesmo código roda no app (sobre o SQLite local, funciona offline) e no
 * servidor (sobre o histórico consolidado de todos os dispositivos), então o
 * número que o usuário vê é idêntico nos dois lados.
 */

export type Period = 'day' | 'week' | 'month' | 'year';

const DAY_MS = 86_400_000;

/** Dia local (YYYY-MM-DD) do evento, respeitando o fuso registrado no evento. */
export function localDay(epochMs: number, tzOffsetMin: number): string {
  return new Date(epochMs + tzOffsetMin * 60_000).toISOString().slice(0, 10);
}

/** Chave de bucket para o período. Semana = ISO week (YYYY-Www). */
export function periodKey(epochMs: number, tzOffsetMin: number, period: Period): string {
  const d = new Date(epochMs + tzOffsetMin * 60_000);
  const iso = d.toISOString();
  switch (period) {
    case 'day':
      return iso.slice(0, 10);
    case 'month':
      return iso.slice(0, 7);
    case 'year':
      return iso.slice(0, 4);
    case 'week': {
      const { year, week } = isoWeek(d);
      return `${year}-W${String(week).padStart(2, '0')}`;
    }
  }
}

function isoWeek(d: Date): { year: number; week: number } {
  const t = new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()));
  const dayNum = t.getUTCDay() || 7;
  t.setUTCDate(t.getUTCDate() + 4 - dayNum);
  const yearStart = Date.UTC(t.getUTCFullYear(), 0, 1);
  return { year: t.getUTCFullYear(), week: Math.ceil(((t.getTime() - yearStart) / DAY_MS + 1) / 7) };
}

/** Considera "escuta" apenas eventos com tempo real ouvido (> 0). */
function counted(e: PlayEvent): boolean {
  return e.listenedMs > 0;
}

/** Uma reprodução conta como "play" após 30 s (mesmo critério da indústria). */
export function isQualifiedPlay(e: PlayEvent): boolean {
  return e.listenedMs >= Math.min(30_000, e.trackDurationMs * 0.5);
}

export function listenedMsByPeriod(events: PlayEvent[], period: Period): Map<string, number> {
  const out = new Map<string, number>();
  for (const e of events) {
    if (!counted(e)) continue;
    const k = periodKey(e.startedAt, e.tzOffsetMin, period);
    out.set(k, (out.get(k) ?? 0) + e.listenedMs);
  }
  return out;
}

export interface RankedItem {
  id: string;
  listenedMs: number;
  plays: number;
}

function rank(events: PlayEvent[], keys: (e: PlayEvent) => string[], limit: number): RankedItem[] {
  const acc = new Map<string, RankedItem>();
  for (const e of events) {
    if (!counted(e)) continue;
    const qualified = isQualifiedPlay(e) ? 1 : 0;
    for (const k of keys(e)) {
      const cur = acc.get(k) ?? { id: k, listenedMs: 0, plays: 0 };
      cur.listenedMs += e.listenedMs;
      cur.plays += qualified;
      acc.set(k, cur);
    }
  }
  return [...acc.values()].sort((a, b) => b.listenedMs - a.listenedMs || b.plays - a.plays).slice(0, limit);
}

export const topTracks = (ev: PlayEvent[], limit = 50) => rank(ev, (e) => [e.trackId], limit);
export const topArtists = (ev: PlayEvent[], limit = 50) => rank(ev, (e) => e.artistIds, limit);
export const topGenres = (ev: PlayEvent[], limit = 20) => rank(ev, (e) => e.genres, limit);

export interface Streak {
  current: number;
  longest: number;
  /** Último dia com escuta (YYYY-MM-DD). */
  lastDay: string | null;
}

/**
 * Streak = dias locais consecutivos com pelo menos `minMsPerDay` ouvidos.
 * O streak atual continua "vivo" se o último dia é hoje ou ontem.
 */
export function streaks(events: PlayEvent[], today: string, minMsPerDay = 60_000): Streak {
  const perDay = listenedMsByPeriod(events, 'day');
  const days = [...perDay.entries()]
    .filter(([, ms]) => ms >= minMsPerDay)
    .map(([d]) => d)
    .sort();
  if (days.length === 0) return { current: 0, longest: 0, lastDay: null };

  let longest = 1;
  let run = 1;
  for (let i = 1; i < days.length; i++) {
    run = dayDiff(days[i - 1]!, days[i]!) === 1 ? run + 1 : 1;
    longest = Math.max(longest, run);
  }
  const lastDay = days[days.length - 1]!;
  const alive = dayDiff(lastDay, today) <= 1;
  return { current: alive ? run : 0, longest, lastDay };
}

function dayDiff(a: string, b: string): number {
  return Math.round((Date.parse(b) - Date.parse(a)) / DAY_MS);
}

/** Matriz 7x24 (dia da semana local x hora local) de ms ouvidos. */
export function listeningClock(events: PlayEvent[]): number[][] {
  const grid = Array.from({ length: 7 }, () => new Array<number>(24).fill(0));
  for (const e of events) {
    if (!counted(e)) continue;
    const d = new Date(e.startedAt + e.tzOffsetMin * 60_000);
    grid[d.getUTCDay()]![d.getUTCHours()]! += e.listenedMs;
  }
  return grid;
}

export interface ListeningSummary {
  from: number;
  to: number;
  totalMs: number;
  totalMinutes: number;
  totalHours: number;
  qualifiedPlays: number;
  skips: number;
  skipRate: number;
  uniqueTracks: number;
  uniqueArtists: number;
  topTracks: RankedItem[];
  topArtists: RankedItem[];
  topGenres: RankedItem[];
  /** Dia com mais escuta no intervalo. */
  peakDay: { day: string; listenedMs: number } | null;
  /** Hora local do dia em que mais ouve (0..23). */
  peakHour: number | null;
  /** Artistas ouvidos pela primeira vez no intervalo. */
  newArtists: ID[];
  streak: Streak;
}

/**
 * "Wrapped contínuo": resumo de qualquer intervalo, a qualquer momento.
 * `history` é o histórico completo (para descobrir artistas novos e streak).
 */
export function summarize(history: PlayEvent[], from: number, to: number, today: string): ListeningSummary {
  const inRange = history.filter((e) => e.startedAt >= from && e.startedAt < to && counted(e));
  const before = new Set<string>();
  for (const e of history) if (e.startedAt < from) e.artistIds.forEach((a) => before.add(a));

  const totalMs = inRange.reduce((s, e) => s + e.listenedMs, 0);
  const qualifiedPlays = inRange.filter(isQualifiedPlay).length;
  const skips = inRange.filter((e) => e.skipped).length;

  const perDay = listenedMsByPeriod(inRange, 'day');
  let peakDay: ListeningSummary['peakDay'] = null;
  for (const [day, ms] of perDay) if (!peakDay || ms > peakDay.listenedMs) peakDay = { day, listenedMs: ms };

  const hours = new Array<number>(24).fill(0);
  for (const row of listeningClock(inRange)) row.forEach((ms, h) => (hours[h]! += ms));
  const peakHour = totalMs > 0 ? hours.indexOf(Math.max(...hours)) : null;

  const artistsInRange = new Set(inRange.flatMap((e) => e.artistIds));
  return {
    from,
    to,
    totalMs,
    totalMinutes: Math.floor(totalMs / 60_000),
    totalHours: Math.round((totalMs / 3_600_000) * 10) / 10,
    qualifiedPlays,
    skips,
    skipRate: inRange.length ? skips / inRange.length : 0,
    uniqueTracks: new Set(inRange.map((e) => e.trackId)).size,
    uniqueArtists: artistsInRange.size,
    topTracks: topTracks(inRange, 10),
    topArtists: topArtists(inRange, 10),
    topGenres: topGenres(inRange, 5),
    peakDay,
    peakHour,
    newArtists: [...artistsInRange].filter((a) => !before.has(a)),
    streak: streaks(history, today),
  };
}

/**
 * Acumulador de tempo ouvido de uma reprodução em andamento.
 * Conta somente tempo de relógio em estado "playing", ignorando pausas e
 * buffering. Seeks não inflam o número porque medimos tempo real, não posição,
 * e a velocidade é respeitada naturalmente (uma faixa de 4 min a 2x = 2 min ouvidos).
 */
export class ListenClock {
  private accumulated = 0;
  private playingSince: number | null = null;

  constructor(private readonly now: () => number = Date.now) {}

  play(): void {
    if (this.playingSince === null) this.playingSince = this.now();
  }

  pause(): void {
    if (this.playingSince !== null) {
      this.accumulated += this.now() - this.playingSince;
      this.playingSince = null;
    }
  }

  /** ms reais de escuta até agora. */
  get listenedMs(): number {
    return this.accumulated + (this.playingSince !== null ? this.now() - this.playingSince : 0);
  }

  reset(): void {
    this.accumulated = 0;
    this.playingSince = null;
  }
}
