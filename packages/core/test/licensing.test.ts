import { describe, expect, it } from 'vitest';
import { authorize, usageCsv, usageReport, type Entitlement } from '../src';
import { ev, track } from './fixtures';

const NOW = Date.UTC(2026, 9, 1);
const premiumBR: Entitlement = { plan: 'premium', validUntil: NOW + 1000, country: 'br' };

describe('licensing', () => {
  it('autoriza por assinatura e território; conteúdo livre sempre toca', () => {
    const licensed = { territories: ['BR', 'PT'] };
    expect(authorize({}, null, NOW).ok).toBe(true);
    expect(authorize(licensed, premiumBR, NOW)).toEqual({ ok: true });
    expect(authorize(licensed, null, NOW)).toEqual({ ok: false, reason: 'no-subscription' });
    expect(authorize(licensed, { ...premiumBR, validUntil: NOW - 1 }, NOW)).toEqual({ ok: false, reason: 'expired' });
    expect(authorize(licensed, { ...premiumBR, country: 'US' }, NOW)).toEqual({ ok: false, reason: 'territory' });
  });

  it('rateia o pool pró-rata por stream qualificado e fecha a soma exata', () => {
    const tracks = new Map([
      ['a', track('a', 'x', { isrc: 'BRAAA2600001', territories: ['BR'], licensor: 'Selo A' })],
      ['b', track('b', 'y', { isrc: 'BRBBB2600002', territories: ['BR'] })],
      ['c', track('c', 'z')], // CC: fora do relatório
    ]);
    const base = { userId: 'u', country: 'BR', plan: 'premium' as const };
    const events = [
      ...Array.from({ length: 2 }, () => ({ ...ev({ startedAt: NOW, trackId: 'a' }), ...base })),
      { ...ev({ startedAt: NOW, trackId: 'b' }), ...base },
      { ...ev({ startedAt: NOW, trackId: 'b', listenedMs: 5000 }), ...base }, // não qualificado
      { ...ev({ startedAt: NOW, trackId: 'c' }), ...base },
    ];
    const rows = usageReport({ events, tracks, royaltyPoolCents: new Map([['BR', 100]]) });
    expect(rows.map((r) => [r.isrc, r.streams, r.revenueShareCents])).toEqual([
      ['BRAAA2600001', 2, 67],
      ['BRBBB2600002', 1, 33],
    ]);
    const csv = usageCsv(rows, '2026-10');
    expect(csv.split('\n')[1]).toBe('2026-10,BRAAA2600001,,a,x,Selo A,BR,SubscriptionModel,OnDemandStream,2,360000,67');
  });
});
