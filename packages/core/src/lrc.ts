/**
 * Parser de letras sincronizadas no formato LRC (linha e "enhanced" por palavra).
 *   [mm:ss.xx] texto
 *   [mm:ss.xx] <mm:ss.xx> pa <mm:ss.xx> la <mm:ss.xx> vra
 *   [offset:+250]
 */

export interface LyricWord {
  timeMs: number;
  text: string;
}

export interface LyricLine {
  timeMs: number;
  text: string;
  words?: LyricWord[];
}

export interface Lyrics {
  synced: boolean;
  lines: LyricLine[];
}

const TIME = /\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\]/g;
const WORD = /<(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?>\s*([^<]*)/g;
const OFFSET = /^\[offset:\s*([+-]?\d+)\s*\]/i;

function toMs(m: string, s: string, frac?: string): number {
  const f = frac ? Number(frac.padEnd(3, '0').slice(0, 3)) : 0;
  return Number(m) * 60_000 + Number(s) * 1000 + f;
}

export function parseLrc(raw: string): Lyrics {
  let offset = 0;
  const lines: LyricLine[] = [];
  const plain: string[] = [];

  for (const rawLine of raw.split(/\r?\n/)) {
    const line = rawLine.trim();
    if (!line) continue;
    const off = OFFSET.exec(line);
    if (off) {
      offset = Number(off[1]);
      continue;
    }
    const stamps = [...line.matchAll(TIME)];
    if (stamps.length === 0) {
      if (!/^\[[a-z]+:/i.test(line)) plain.push(line); // ignora tags [ar:], [ti:]...
      continue;
    }
    const body = line.replace(TIME, '').trim();
    const words = [...body.matchAll(WORD)].map((w) => ({ timeMs: toMs(w[1]!, w[2]!, w[3]), text: w[4]!.trim() }));
    const text = words.length ? words.map((w) => w.text).filter(Boolean).join(' ') : body;
    // Uma linha pode ter vários timestamps (refrão repetido).
    for (const st of stamps) lines.push({ timeMs: toMs(st[1]!, st[2]!, st[3]), text, ...(words.length ? { words } : {}) });
  }

  if (lines.length === 0) return { synced: false, lines: plain.map((text) => ({ timeMs: 0, text })) };

  // LRC: offset positivo = letra aparece mais cedo.
  const shift = (t: number) => Math.max(0, t - offset);
  lines.sort((a, b) => a.timeMs - b.timeMs);
  return {
    synced: true,
    lines: lines.map((l) => ({
      ...l,
      timeMs: shift(l.timeMs),
      ...(l.words ? { words: l.words.map((w) => ({ ...w, timeMs: shift(w.timeMs) })) } : {}),
    })),
  };
}

/** Índice da linha ativa para a posição atual (busca binária, O(log n) por frame). */
export function activeLineIndex(lines: LyricLine[], positionMs: number): number {
  let lo = 0;
  let hi = lines.length - 1;
  let ans = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (lines[mid]!.timeMs <= positionMs) {
      ans = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  return ans;
}
