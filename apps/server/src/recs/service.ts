import { buildProfile, dailyMixClusters, rankCandidates, smartShuffle, topTracks, type Candidate, type Track } from '@sonora/core';
import type { CatalogService } from '../providers/catalog';
import type { Repos } from '../repos/types';

export interface Mix {
  id: string;
  title: string;
  subtitle: string;
  tracks: Track[];
}

const WEEK = 7 * 86_400_000;

/**
 * Gera as playlists algorítmicas. Determinísticas por período (a semente é a
 * semana/dia), então o Discover Weekly não muda a cada abertura e é igual em
 * todos os dispositivos.
 */
export class RecommendationService {
  constructor(
    private readonly repos: Repos,
    private readonly catalog: CatalogService,
  ) {}

  private async context(userId: string, now: number) {
    const history = await this.repos.plays.list(userId, now - 180 * 86_400_000, now + 1);
    const profile = buildProfile(history, now);
    const favorites = await this.catalog.tracks(topTracks(history, 60).map((r) => r.id));
    return { history, profile, favorites };
  }

  private async candidatesFrom(seeds: Track[], profile: ReturnType<typeof buildProfile>, perSeed = 15): Promise<Candidate[]> {
    const out: Candidate[] = [];
    await Promise.all(
      seeds.slice(0, 20).map(async (seed) => {
        const neigh = await this.repos.neighbors.of(seed.id, perSeed);
        const max = neigh[0]?.score ?? 1;
        const ts = await this.catalog.tracks(neigh.map((n) => n.id));
        ts.forEach((t, i) => out.push({ track: t, cooc: (neigh[i]?.score ?? 0) / max }));
      }),
    );
    // Candidatos por gênero cobrem cold start e catálogos sem dados de co-escuta.
    const genres = [...profile.genres.entries()].filter(([, v]) => v > 0).sort((a, b) => b[1] - a[1]).slice(0, 4).map(([g]) => g);
    const fallbackGenres = genres.length ? genres : [...new Set(seeds.flatMap((s) => s.genres))].slice(0, 4);
    const byGenre = await Promise.all(fallbackGenres.map((g) => this.catalog.tracksByGenre(g, 40).catch(() => [])));
    for (const list of byGenre) for (const t of list) out.push({ track: t });
    return out;
  }

  async discoverWeekly(userId: string, now = Date.now()): Promise<Mix> {
    const { profile, favorites } = await this.context(userId, now);
    const cands = (await this.candidatesFrom(favorites, profile)).filter((c) => !profile.tracks.has(c.track.id));
    const week = Math.floor(now / WEEK);
    const tracks = smartShuffle(rankCandidates(cands, profile, { discovery: 0.9, excludeRecent: true, limit: 30, maxPerArtist: 1 }), week);
    return { id: `discover-weekly:${week}`, title: 'Descobertas da Semana', subtitle: 'Músicas novas para você, toda segunda-feira', tracks };
  }

  async dailyMixes(userId: string, now = Date.now()): Promise<Mix[]> {
    const { profile, favorites } = await this.context(userId, now);
    const day = Math.floor(now / 86_400_000);
    const clusters = dailyMixClusters(favorites, profile, 6);
    return Promise.all(
      clusters.map(async (c, i) => {
        const fresh = await this.candidatesFrom(c.seeds, profile, 8);
        const familiar = rankCandidates(c.seeds.map((t) => ({ track: t, cooc: 1 })), profile, { discovery: 0, limit: 35, maxPerArtist: 4 });
        const discovery = rankCandidates(fresh.filter((x) => x.track.genres.includes(c.genre)), profile, { discovery: 0.8, limit: 15, excludeRecent: true });
        return {
          id: `daily-mix:${day}:${i + 1}`,
          title: `Daily Mix ${i + 1}`,
          subtitle: `${c.genre} · ${[...new Set(c.seeds.map((s) => s.artists[0]?.name))].slice(0, 3).join(', ')}`,
          tracks: smartShuffle([...familiar, ...discovery], day + i),
        };
      }),
    );
  }

  /** Rádio infinito a partir de uma faixa ou artista; o app pede mais quando a fila acaba. */
  async radio(userId: string, seedTrackId: string, exclude: string[] = [], now = Date.now()): Promise<Track[]> {
    const [seed, { profile }] = await Promise.all([this.catalog.track(seedTrackId), this.context(userId, now)]);
    if (!seed) return [];
    const skip = new Set([seedTrackId, ...exclude]);
    const cands = (await this.candidatesFrom([seed], profile, 40)).filter((c) => !skip.has(c.track.id));
    return rankCandidates(cands, profile, { discovery: 0.5, limit: 25, maxPerArtist: 2 });
  }

  /** Recomendações para completar uma playlist ("Músicas recomendadas" no fim da playlist). */
  async forPlaylist(userId: string, trackIds: string[], now = Date.now()): Promise<Track[]> {
    const [seeds, { profile }] = await Promise.all([this.catalog.tracks(trackIds.slice(-20)), this.context(userId, now)]);
    const inPlaylist = new Set(trackIds);
    const cands = (await this.candidatesFrom(seeds, profile, 10)).filter((c) => !inPlaylist.has(c.track.id));
    return rankCandidates(cands, profile, { discovery: 0.6, limit: 10, maxPerArtist: 1 });
  }
}
