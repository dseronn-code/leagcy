import { cooccurrence, sessionize } from '@sonora/core';
import type { Repos } from '../repos/types';

/**
 * Job periódico (ex.: a cada hora): recalcula vizinhos de co-escuta a partir
 * das sessões de todos os usuários nos últimos 90 dias. Normaliza por cosseno
 * para não favorecer só os hits, e guarda os top-K por faixa.
 */
export async function rebuildNeighbors(repos: Repos, now = Date.now(), topK = 50): Promise<number> {
  const events = await repos.plays.allSince(now - 90 * 86_400_000);
  const byUser = new Map<string, typeof events>();
  for (const e of events) {
    const list = byUser.get(e.userId) ?? [];
    list.push(e);
    byUser.set(e.userId, list);
  }
  const sessions = [...byUser.values()].flatMap((evs) => sessionize(evs));
  const m = cooccurrence(sessions);
  const freq = new Map<string, number>();
  for (const s of sessions) for (const t of s) freq.set(t, (freq.get(t) ?? 0) + 1);

  for (const [track, row] of m) {
    const scored = [...row.entries()]
      .map(([id, c]) => ({ id, score: c / Math.sqrt((freq.get(track) ?? 1) * (freq.get(id) ?? 1)) }))
      .sort((a, b) => b.score - a.score)
      .slice(0, topK);
    await repos.neighbors.replace(track, scored);
  }
  return m.size;
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const { postgresRepos } = await import('../repos/postgres');
  const url = process.env.DATABASE_URL;
  if (!url) throw new Error('DATABASE_URL é obrigatório');
  const repos = postgresRepos(url);
  console.log(`vizinhos recalculados para ${await rebuildNeighbors(repos)} faixas`);
  await repos.close();
}
