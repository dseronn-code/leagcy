/**
 * Chaves de ordenação fracionárias (base 62) para playlists.
 *
 * Mover uma faixa gera UMA nova chave entre os vizinhos, sem reescrever as
 * outras posições. Isso torna arrastar-e-soltar O(1) no banco e permite que
 * dois colaboradores reordenem a mesma playlist offline sem conflito
 * (no pior caso duas entradas ficam com chaves iguais e desempatamos por entryId).
 */

const DIGITS = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
const BASE = DIGITS.length;

function digit(c: string): number {
  const d = DIGITS.indexOf(c);
  if (d < 0) throw new Error(`caractere inválido na chave: ${c}`);
  return d;
}

/**
 * Gera uma chave estritamente entre `a` e `b` (null = extremo aberto).
 * Requer a < b quando ambos existem.
 */
export function keyBetween(a: string | null, b: string | null): string {
  if (a !== null && b !== null && a >= b) throw new Error(`keyBetween: ${a} >= ${b}`);
  const lo = a ?? '';
  const hi = b;
  let out = '';
  for (let i = 0; ; i++) {
    const dl = i < lo.length ? digit(lo[i]!) : 0;
    const dh = hi !== null && i < hi.length ? digit(hi[i]!) : BASE;
    if (dl === dh) {
      out += DIGITS[dl];
      continue;
    }
    const mid = Math.floor((dl + dh) / 2);
    if (mid > dl) {
      // Evita terminar em '0' para que sempre exista espaço antes da chave.
      return out + DIGITS[mid];
    }
    // dh = dl + 1: copia o dígito de baixo e continua procurando sem limite superior.
    out += DIGITS[dl];
    return out + keyBetween(lo.slice(i + 1) || null, null);
  }
}

/** Gera `n` chaves crescentes entre a e b (para inserir vários itens de uma vez). */
export function keysBetween(a: string | null, b: string | null, n: number): string[] {
  const keys: string[] = [];
  let prev = a;
  for (let i = 0; i < n; i++) {
    const k = keyBetween(prev, b);
    keys.push(k);
    prev = k;
  }
  return keys;
}

export function compareEntries(a: { position: string; entryId: string }, b: { position: string; entryId: string }): number {
  if (a.position < b.position) return -1;
  if (a.position > b.position) return 1;
  return a.entryId < b.entryId ? -1 : a.entryId > b.entryId ? 1 : 0;
}
