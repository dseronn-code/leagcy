import type { Mix } from '@/api/client';

/** Mixes algorítmicos vêm prontos do /recs/home; guardamos em memória para a tela do mix. */
const mixes = new Map<string, Mix>();

export function cacheMix(m: Mix) {
  mixes.set(m.id, m);
}

export function getMix(id: string): Mix | undefined {
  return mixes.get(id);
}
