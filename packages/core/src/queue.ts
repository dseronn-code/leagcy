import type { ID, RepeatMode } from './types';

/**
 * Motor de fila puro e imutável (sem dependência de player nativo).
 *
 * Modelo inspirado no Spotify:
 *  - `context`: faixas do contexto atual (álbum, playlist...) na ordem original.
 *  - `order`: permutação de índices de `context` (identidade ou embaralhada).
 *  - `userQueue`: faixas adicionadas com "Tocar a seguir"/"Adicionar à fila";
 *    sempre tocam antes de continuar o contexto e nunca são embaralhadas.
 */
export interface QueueState {
  context: ID[];
  order: number[];
  /** Posição atual dentro de `order`. */
  cursor: number;
  userQueue: ID[];
  /** Faixa vinda da userQueue que está tocando agora (se houver). */
  playingFromUserQueue: ID | null;
  shuffle: boolean;
  repeat: RepeatMode;
  /** Semente para shuffle determinístico (reprodutível entre dispositivos). */
  seed: number;
}

export function createQueue(context: ID[], startIndex = 0, opts: Partial<Pick<QueueState, 'shuffle' | 'repeat' | 'seed'>> = {}): QueueState {
  const base: QueueState = {
    context,
    order: context.map((_, i) => i),
    cursor: Math.min(Math.max(startIndex, 0), Math.max(context.length - 1, 0)),
    userQueue: [],
    playingFromUserQueue: null,
    shuffle: false,
    repeat: opts.repeat ?? 'off',
    seed: opts.seed ?? 1,
  };
  return opts.shuffle ? setShuffle(base, true) : base;
}

export function currentTrack(q: QueueState): ID | null {
  if (q.playingFromUserQueue) return q.playingFromUserQueue;
  const idx = q.order[q.cursor];
  return idx === undefined ? null : (q.context[idx] ?? null);
}

/** Próximas N faixas na ordem em que realmente vão tocar. */
export function upNext(q: QueueState, limit = 50): ID[] {
  const out: ID[] = q.userQueue.slice(0, limit);
  const len = q.order.length;
  // Com repeat 'all' mostra uma volta completa (inclusive a atual, que repete no fim).
  const steps = q.repeat === 'all' ? len : len - q.cursor - 1;
  for (let s = 1; s <= steps && out.length < limit; s++) {
    out.push(q.context[q.order[(q.cursor + s) % len]!]!);
  }
  return out;
}

/**
 * Avança. `auto` = fim natural da faixa (respeita repeat 'one');
 * botão "próxima" passa `auto=false` e sempre muda de faixa.
 */
export function next(q: QueueState, auto = false): QueueState | null {
  if (auto && q.repeat === 'one' && currentTrack(q)) return q;
  if (q.userQueue.length > 0) {
    const [head, ...rest] = q.userQueue;
    return { ...q, userQueue: rest, playingFromUserQueue: head! };
  }
  const base = { ...q, playingFromUserQueue: null };
  // Se tocava da userQueue, o cursor ainda aponta para a última faixa do contexto já tocada.
  const nextCursor = q.cursor + 1;
  if (nextCursor < q.order.length) return { ...base, cursor: nextCursor };
  if (q.repeat === 'all' && q.order.length > 0) {
    // Ao recomeçar com shuffle, gera nova permutação para não repetir a mesma ordem.
    if (q.shuffle) {
      const seed = q.seed + 1;
      return { ...base, seed, order: shuffled(q.order.length, seed), cursor: 0 };
    }
    return { ...base, cursor: 0 };
  }
  return null;
}

/** Volta. Se já passou de 3 s na faixa, o player deve reiniciar a faixa em vez de chamar isto. */
export function previous(q: QueueState): QueueState {
  if (q.playingFromUserQueue) return { ...q, playingFromUserQueue: null };
  if (q.cursor > 0) return { ...q, cursor: q.cursor - 1 };
  if (q.repeat === 'all' && q.order.length > 0) return { ...q, cursor: q.order.length - 1 };
  return q;
}

export function jumpToContextIndex(q: QueueState, contextIndex: number): QueueState {
  const cursor = q.order.indexOf(contextIndex);
  return cursor < 0 ? q : { ...q, cursor, playingFromUserQueue: null };
}

export function playNext(q: QueueState, ids: ID[]): QueueState {
  return { ...q, userQueue: [...ids, ...q.userQueue] };
}

export function addToQueue(q: QueueState, ids: ID[]): QueueState {
  return { ...q, userQueue: [...q.userQueue, ...ids] };
}

export function removeFromUserQueue(q: QueueState, index: number): QueueState {
  return { ...q, userQueue: q.userQueue.filter((_, i) => i !== index) };
}

/** Arrastar e soltar dentro da fila do usuário. */
export function moveInUserQueue(q: QueueState, from: number, to: number): QueueState {
  return { ...q, userQueue: move(q.userQueue, from, to) };
}

/**
 * Arrastar e soltar no "a seguir" do contexto. `from`/`to` são offsets relativos
 * à faixa atual (0 = próxima faixa do contexto).
 */
export function moveInUpcomingContext(q: QueueState, from: number, to: number): QueueState {
  const head = q.order.slice(0, q.cursor + 1);
  const tail = move(q.order.slice(q.cursor + 1), from, to);
  return { ...q, order: [...head, ...tail] };
}

export function setRepeat(q: QueueState, repeat: RepeatMode): QueueState {
  return { ...q, repeat };
}

export function cycleRepeat(q: QueueState): QueueState {
  const nextMode: Record<RepeatMode, RepeatMode> = { off: 'all', all: 'one', one: 'off' };
  return setRepeat(q, nextMode[q.repeat]);
}

/**
 * Liga/desliga shuffle mantendo a faixa atual tocando:
 * a atual vira o primeiro item da nova ordem; ao desligar, volta à ordem original
 * a partir da posição da faixa atual.
 */
export function setShuffle(q: QueueState, on: boolean): QueueState {
  if (q.context.length === 0) return { ...q, shuffle: on };
  const currentIdx = q.order[q.cursor] ?? 0;
  if (on) {
    const rest = shuffled(q.context.length, q.seed).filter((i) => i !== currentIdx);
    return { ...q, shuffle: true, order: [currentIdx, ...rest], cursor: 0 };
  }
  return { ...q, shuffle: false, order: q.context.map((_, i) => i), cursor: currentIdx };
}

function move<T>(arr: T[], from: number, to: number): T[] {
  if (from < 0 || from >= arr.length) return arr;
  const copy = arr.slice();
  const [item] = copy.splice(from, 1);
  copy.splice(Math.max(0, Math.min(to, copy.length)), 0, item!);
  return copy;
}

/** PRNG mulberry32: rápido e determinístico. */
export function mulberry32(seed: number): () => number {
  let a = seed >>> 0;
  return () => {
    a = (a + 0x6d2b79f5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Fisher–Yates determinístico sobre [0..n). */
export function shuffled(n: number, seed: number): number[] {
  const rand = mulberry32(seed);
  const a = Array.from({ length: n }, (_, i) => i);
  for (let i = n - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [a[i], a[j]] = [a[j]!, a[i]!];
  }
  return a;
}
