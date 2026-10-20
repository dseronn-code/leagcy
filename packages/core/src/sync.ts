/**
 * Sincronização entre dispositivos.
 *
 * Cada mudança local vira uma `ChangeOp` carimbada com um Hybrid Logical Clock
 * (HLC). O servidor guarda o log e cada dispositivo puxa as ops desde o seu
 * último cursor. A resolução é last-writer-wins por (entidade, campo), com o
 * HLC como ordem total — determinística em todos os dispositivos, sem depender
 * de relógios sincronizados.
 *
 * Para coleções (curtidas, entradas de playlist) usamos ops de add/remove por
 * elemento, também LWW: o elemento existe se o último op dele foi "add".
 * O histórico de escuta é append-only (sem conflito).
 */

export interface HLC {
  /** ms físicos */
  wall: number;
  /** contador lógico */
  counter: number;
  node: string;
}

export function hlcToString(h: HLC): string {
  // Ordenável lexicograficamente.
  return `${h.wall.toString(36).padStart(9, '0')}:${h.counter.toString(36).padStart(5, '0')}:${h.node}`;
}

export function hlcParse(s: string): HLC {
  const [w, c, ...node] = s.split(':');
  return { wall: parseInt(w!, 36), counter: parseInt(c!, 36), node: node.join(':') };
}

export class HybridClock {
  private last: HLC;

  constructor(node: string, private readonly now: () => number = Date.now) {
    this.last = { wall: 0, counter: 0, node };
  }

  /** Carimbo para um evento local. */
  tick(): string {
    const pt = this.now();
    if (pt > this.last.wall) this.last = { ...this.last, wall: pt, counter: 0 };
    else this.last = { ...this.last, counter: this.last.counter + 1 };
    return hlcToString(this.last);
  }

  /** Atualiza ao receber um carimbo remoto. */
  receive(remote: string): void {
    const r = hlcParse(remote);
    const pt = this.now();
    const wall = Math.max(pt, this.last.wall, r.wall);
    let counter: number;
    if (wall === this.last.wall && wall === r.wall) counter = Math.max(this.last.counter, r.counter) + 1;
    else if (wall === this.last.wall) counter = this.last.counter + 1;
    else if (wall === r.wall) counter = r.counter + 1;
    else counter = 0;
    this.last = { wall, counter, node: this.last.node };
  }
}

export type EntityType = 'playlist' | 'playlistEntry' | 'like' | 'savedAlbum' | 'followArtist' | 'followPlaylist' | 'settings' | 'playback';

export interface ChangeOp {
  entity: EntityType;
  entityId: string;
  /** Campo alterado; '$exists' para add/remove de membros de coleções. */
  field: string;
  value: unknown;
  hlc: string;
  deviceId: string;
}

export type EntityState = Record<string, { value: unknown; hlc: string }>;
export type Store = Map<string, EntityState>;

const keyOf = (op: Pick<ChangeOp, 'entity' | 'entityId'>) => `${op.entity}/${op.entityId}`;

/** Aplica ops (em qualquer ordem, idempotente) — merge comutativo e associativo. */
export function applyOps(store: Store, ops: Iterable<ChangeOp>): Store {
  for (const op of ops) {
    const k = keyOf(op);
    const state = store.get(k) ?? {};
    const cur = state[op.field];
    if (!cur || op.hlc > cur.hlc) {
      state[op.field] = { value: op.value, hlc: op.hlc };
      store.set(k, state);
    }
  }
  return store;
}

export function materialize(store: Store, entity: EntityType): Array<{ id: string } & Record<string, unknown>> {
  const out: Array<{ id: string } & Record<string, unknown>> = [];
  const prefix = `${entity}/`;
  for (const [k, state] of store) {
    if (!k.startsWith(prefix)) continue;
    if (state.$exists && state.$exists.value === false) continue;
    const obj: { id: string } & Record<string, unknown> = { id: k.slice(prefix.length) };
    for (const [f, v] of Object.entries(state)) if (f !== '$exists') obj[f] = v.value;
    out.push(obj);
  }
  return out;
}
