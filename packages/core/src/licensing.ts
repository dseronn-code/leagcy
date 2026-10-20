import type { PlayEvent, Track } from './types';
import { isQualifiedPlay } from './stats';

/**
 * Regras exigidas por licenciantes (gravadoras, distribuidoras, editoras).
 * Funções puras, usadas pelo servidor na autorização de stream e na
 * geração dos relatórios de uso que embasam o pagamento de royalties.
 */

export type Plan = 'none' | 'trial' | 'premium' | 'family' | 'student';

export interface Entitlement {
  plan: Plan;
  /** epoch ms; assinatura vencida não toca catálogo licenciado. */
  validUntil: number;
  /** País da conta (ISO alfa-2), usado para território. */
  country: string;
}

export type DenyReason = 'no-subscription' | 'expired' | 'territory' | 'not-licensed';

export function isLicensed(t: Pick<Track, 'territories'>): boolean {
  return t.territories !== undefined;
}

/** Pode tocar/baixar esta faixa? Conteúdo não licenciado (CC, acervo próprio) é livre. */
export function authorize(t: Pick<Track, 'territories'>, e: Entitlement | null, now = Date.now()): { ok: true } | { ok: false; reason: DenyReason } {
  if (!isLicensed(t)) return { ok: true };
  if (!e || e.plan === 'none') return { ok: false, reason: 'no-subscription' };
  if (e.validUntil < now) return { ok: false, reason: 'expired' };
  if (t.territories!.length === 0) return { ok: false, reason: 'not-licensed' };
  if (!t.territories!.includes(e.country.toUpperCase())) return { ok: false, reason: 'territory' };
  return { ok: true };
}

/** Prazo padrão de licença offline: o app precisa revalidar online a cada 30 dias. */
export const OFFLINE_LICENSE_MS = 30 * 86_400_000;

export interface UsageRow {
  isrc: string;
  upc: string;
  trackId: string;
  title: string;
  artist: string;
  licensor: string;
  country: string;
  plan: Plan;
  /** Reproduções qualificadas (≥ 30 s). Base do cálculo pró-rata. */
  streams: number;
  listenedMs: number;
  /** Fatia da receita líquida atribuída, em centavos. */
  revenueShareCents: number;
}

export interface UsageInput {
  events: Array<PlayEvent & { userId: string; country: string; plan: Plan }>;
  tracks: Map<string, Track>;
  /** Receita líquida do período destinada a royalties (centavos), por país. */
  royaltyPoolCents: Map<string, number>;
}

/**
 * Relatório de uso por (ISRC, país, plano) com rateio pró-rata por stream
 * qualificado dentro de cada país — o modelo mais comum nos contratos.
 * Só entram faixas licenciadas (com ISRC/territórios); arredondamento por
 * maior resto garante que a soma bate exatamente com o pool.
 */
export function usageReport({ events, tracks, royaltyPoolCents }: UsageInput): UsageRow[] {
  const rows = new Map<string, UsageRow>();
  for (const e of events) {
    const t = tracks.get(e.trackId);
    if (!t || !isLicensed(t) || !t.isrc) continue;
    const key = `${t.isrc}|${e.country}|${e.plan}`;
    const r = rows.get(key) ?? {
      isrc: t.isrc,
      upc: t.upc ?? '',
      trackId: t.id,
      title: t.title,
      artist: t.artists.map((a) => a.name).join(', '),
      licensor: t.licensor ?? '',
      country: e.country,
      plan: e.plan,
      streams: 0,
      listenedMs: 0,
      revenueShareCents: 0,
    };
    if (isQualifiedPlay(e)) r.streams++;
    r.listenedMs += e.listenedMs;
    rows.set(key, r);
  }

  const byCountry = new Map<string, UsageRow[]>();
  for (const r of rows.values()) byCountry.set(r.country, [...(byCountry.get(r.country) ?? []), r]);
  for (const [country, list] of byCountry) {
    const pool = royaltyPoolCents.get(country) ?? 0;
    const total = list.reduce((s, r) => s + r.streams, 0);
    if (!pool || !total) continue;
    const exact = list.map((r) => (pool * r.streams) / total);
    list.forEach((r, i) => (r.revenueShareCents = Math.floor(exact[i]!)));
    let rest = pool - list.reduce((s, r) => s + r.revenueShareCents, 0);
    const order = list.map((_, i) => i).sort((a, b) => (exact[b]! % 1) - (exact[a]! % 1));
    for (const i of order) {
      if (rest <= 0) break;
      list[i]!.revenueShareCents++;
      rest--;
    }
  }
  return [...rows.values()].filter((r) => r.streams > 0 || r.listenedMs > 0).sort((a, b) => b.streams - a.streams || a.isrc.localeCompare(b.isrc));
}

const csvCell = (v: string | number) => {
  const s = String(v);
  return /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
};

/** CSV com as colunas que distribuidoras pedem; base para converter em DDEX DSR. */
export function usageCsv(rows: UsageRow[], period: string): string {
  const header = ['period', 'isrc', 'upc', 'title', 'artist', 'licensor', 'territory', 'commercial_model', 'use_type', 'streams', 'listened_ms', 'revenue_cents'];
  const lines = rows.map((r) =>
    [period, r.isrc, r.upc, r.title, r.artist, r.licensor, r.country, 'SubscriptionModel', r.plan === 'trial' ? 'TrialStream' : 'OnDemandStream', r.streams, r.listenedMs, r.revenueShareCents].map(csvCell).join(','),
  );
  return [header.join(','), ...lines].join('\n') + '\n';
}
