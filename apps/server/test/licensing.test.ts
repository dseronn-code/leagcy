import { afterAll, beforeAll, describe, expect, it } from 'vitest';
import type { Album, ArtistPage, AudioSource, Track } from '@sonora/core';
import { buildApp } from '../src/app';
import { LyricsService } from '../src/lyrics';
import { CatalogService } from '../src/providers/catalog';
import type { CatalogProvider, StreamContext } from '../src/providers/types';
import { RecommendationService } from '../src/recs/service';
import { memoryRepos } from '../src/repos/memory';

const licensed: Track = {
  id: 'lic:1',
  provider: 'lic',
  title: 'Faixa Licenciada',
  artists: [{ id: 'lic:a', name: 'Artista' }],
  albumId: 'lic:al',
  albumTitle: 'Álbum',
  durationMs: 200_000,
  genres: ['mpb'],
  isrc: 'BRXXX2600001',
  upc: '7890000000001',
  licensor: 'Selo X',
  territories: ['BR'],
  sources: [{ url: '', codec: 'flac', bitrate: 4000, sampleRateHz: 96_000, bitDepth: 24, channels: 2, requiresAuthorization: true }],
};

const signed: StreamContext[] = [];

class LicensedFake implements CatalogProvider {
  readonly id = 'lic';
  async search() {
    return { tracks: [licensed], albums: [] as Album[], artists: [] };
  }
  async track(raw: string) {
    return raw === '1' ? licensed : null;
  }
  async album() {
    return null;
  }
  async artist(): Promise<ArtistPage | null> {
    return null;
  }
  async tracksByGenre() {
    return [];
  }
  async streamSources(_raw: string, ctx: StreamContext): Promise<AudioSource[]> {
    signed.push(ctx);
    return [{ ...licensed.sources[0]!, url: `https://cdn.example/1.flac?sig=${ctx.userId}`, requiresAuthorization: false }];
  }
}

const repos = memoryRepos();
const catalog = new CatalogService([new LicensedFake()]);
let app: Awaited<ReturnType<typeof buildApp>>;
const ADMIN = { 'x-admin-token': 'segredo-admin' };

async function register(email: string, country: string) {
  const r = await app.inject({ method: 'POST', url: '/auth/register', payload: { email, password: 'senha-forte-123', country } });
  const h = { authorization: `Bearer ${r.json().token}` };
  const me = (await app.inject({ url: '/me', headers: h })).json();
  return { h, id: me.id as string, me };
}

beforeAll(async () => {
  app = await buildApp({ repos, catalog, lyrics: new LyricsService('http://127.0.0.1:9'), recs: new RecommendationService(repos, catalog), jwtSecret: 'test', adminToken: ADMIN['x-admin-token'] });
});
afterAll(() => app.close());

describe('licenciamento', () => {
  it('sem assinatura não recebe URL; com assinatura no território recebe URL assinada', async () => {
    const u = await register('l1@x.com', 'br');
    expect(u.me.entitlement).toMatchObject({ plan: 'none', country: 'BR' });
    const denied = await app.inject({ url: '/tracks/lic:1/stream?deviceId=d1', headers: u.h });
    expect(denied.statusCode).toBe(403);
    expect(denied.json().error).toBe('no-subscription');

    await app.inject({ method: 'POST', url: `/admin/users/${u.id}/entitlement`, headers: ADMIN, payload: { plan: 'premium', validUntil: Date.now() + 86_400_000, country: 'BR' } });
    const ok = await app.inject({ url: '/tracks/lic:1/stream?deviceId=d1&purpose=download', headers: u.h });
    expect(ok.statusCode).toBe(200);
    expect(ok.json().sources[0].url).toContain(`sig=${u.id}`);
    expect(ok.json().licenseExpiresAt).toBeGreaterThan(Date.now());
    expect(signed.at(-1)).toMatchObject({ deviceId: 'd1', country: 'BR', purpose: 'download' });
  });

  it('bloqueia fora do território e revoga downloads na renovação', async () => {
    const u = await register('l2@x.com', 'US');
    await app.inject({ method: 'POST', url: `/admin/users/${u.id}/entitlement`, headers: ADMIN, payload: { plan: 'premium', validUntil: Date.now() + 86_400_000, country: 'US' } });
    expect((await app.inject({ url: '/tracks/lic:1/stream?deviceId=d', headers: u.h })).json().error).toBe('territory');
    const renew = (await app.inject({ method: 'POST', url: '/downloads/renew', headers: u.h, payload: { ids: ['lic:1', 'lic:sumiu'] } })).json();
    expect(renew.revoked).toEqual(['lic:1', 'lic:sumiu']);
  });

  it('rotas admin ficam ocultas sem token e o relatório sai em CSV com rateio', async () => {
    expect((await app.inject({ url: '/admin/reports/usage?period=2026-10' })).statusCode).toBe(404);
    const u = await register('l3@x.com', 'BR');
    await app.inject({ method: 'POST', url: `/admin/users/${u.id}/entitlement`, headers: ADMIN, payload: { plan: 'premium', validUntil: Date.now() + 86_400_000, country: 'BR' } });
    const start = Date.UTC(2026, 9, 10);
    const events = [0, 1, 2].map((i) => ({
      id: `lic-evt-${i}-xxxx`,
      trackId: 'lic:1',
      artistIds: ['lic:a'],
      genres: ['mpb'],
      startedAt: start + i * 300_000,
      listenedMs: i === 2 ? 4000 : 190_000,
      trackDurationMs: 200_000,
      skipped: i === 2,
      source: { type: 'library' },
      deviceId: 'd',
      tzOffsetMin: -180,
    }));
    await app.inject({ method: 'POST', url: '/plays', headers: u.h, payload: { events } });
    const r = await app.inject({ url: '/admin/reports/usage?period=2026-10&pool=BR:1000', headers: ADMIN });
    expect(r.headers['content-type']).toContain('text/csv');
    const lines = r.body.trim().split('\n');
    expect(lines).toHaveLength(2);
    expect(lines[1]).toBe('2026-10,BRXXX2600001,7890000000001,Faixa Licenciada,Artista,Selo X,BR,SubscriptionModel,OnDemandStream,2,384000,1000');
  });
});
