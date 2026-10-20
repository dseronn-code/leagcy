import { describe, expect, it } from 'vitest';
import { ListenClock, listenedMsByPeriod, periodKey, streaks, summarize, topArtists } from '../src';
import { ev } from './fixtures';

const D = 86_400_000;
const T0 = Date.UTC(2026, 0, 5, 12); // segunda-feira

describe('stats', () => {
  it('agrupa por dia local usando o fuso do evento', () => {
    // 01:30 UTC = 22:30 do dia anterior em UTC-3
    const e = ev({ startedAt: Date.UTC(2026, 0, 6, 1, 30), tzOffsetMin: -180 });
    expect(periodKey(e.startedAt, e.tzOffsetMin, 'day')).toBe('2026-01-05');
    expect(periodKey(T0, 0, 'week')).toBe('2026-W02');
  });

  it('soma minutos por período', () => {
    const events = [ev({ startedAt: T0 }), ev({ startedAt: T0 + 1000, listenedMs: 60_000 }), ev({ startedAt: T0 + D })];
    const days = listenedMsByPeriod(events, 'day');
    expect(days.get('2026-01-05')).toBe(240_000);
    expect(listenedMsByPeriod(events, 'month').get('2026-01')).toBe(420_000);
  });

  it('calcula streaks atual e mais longo', () => {
    const days = [0, 1, 2, 5, 6];
    const events = days.map((d) => ev({ startedAt: T0 + d * D }));
    expect(streaks(events, '2026-01-11')).toEqual({ current: 2, longest: 3, lastDay: '2026-01-11' });
    expect(streaks(events, '2026-01-14').current).toBe(0);
  });

  it('ranqueia artistas por tempo ouvido', () => {
    const events = [ev({ startedAt: T0, artistIds: ['x'], listenedMs: 10_000 }), ev({ startedAt: T0, artistIds: ['y'] })];
    expect(topArtists(events).map((r) => r.id)).toEqual(['y', 'x']);
  });

  it('resume intervalo com artistas novos', () => {
    const history = [ev({ startedAt: T0 - 10 * D, artistIds: ['old'] }), ev({ startedAt: T0, artistIds: ['old'] }), ev({ startedAt: T0, artistIds: ['new'] })];
    const s = summarize(history, T0 - D, T0 + D, '2026-01-05');
    expect(s.totalMinutes).toBe(6);
    expect(s.newArtists).toEqual(['new']);
    expect(s.peakHour).toBe(12);
  });

  it('ListenClock ignora pausas', () => {
    let now = 0;
    const c = new ListenClock(() => now);
    c.play();
    now = 5000;
    c.pause();
    now = 60_000;
    c.play();
    now = 62_000;
    expect(c.listenedMs).toBe(7000);
  });
});
