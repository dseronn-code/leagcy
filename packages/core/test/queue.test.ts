import { describe, expect, it } from 'vitest';
import { addToQueue, createQueue, currentTrack, cycleRepeat, moveInUpcomingContext, next, playNext, previous, setShuffle, upNext } from '../src';

const ctx = ['a', 'b', 'c', 'd', 'e'];

describe('queue', () => {
  it('toca o contexto em ordem e termina sem repeat', () => {
    let q = createQueue(ctx, 3);
    expect(currentTrack(q)).toBe('d');
    q = next(q)!;
    expect(currentTrack(q)).toBe('e');
    expect(next(q)).toBeNull();
  });

  it('fila do usuário tem prioridade e depois retoma o contexto', () => {
    let q = createQueue(ctx, 0);
    q = addToQueue(q, ['x']);
    q = playNext(q, ['y']);
    expect(upNext(q, 4)).toEqual(['y', 'x', 'b', 'c']);
    q = next(q)!;
    expect(currentTrack(q)).toBe('y');
    q = next(q)!;
    expect(currentTrack(q)).toBe('x');
    q = next(q)!;
    expect(currentTrack(q)).toBe('b');
  });

  it('repeat one só repete no fim natural', () => {
    let q = cycleRepeat(cycleRepeat(createQueue(ctx)));
    expect(q.repeat).toBe('one');
    expect(currentTrack(next(q, true)!)).toBe('a');
    expect(currentTrack(next(q, false)!)).toBe('b');
  });

  it('repeat all volta ao início', () => {
    const q = { ...createQueue(ctx, 4), repeat: 'all' as const };
    expect(currentTrack(next(q)!)).toBe('a');
    expect(currentTrack(previous(createQueue(ctx, 0, { repeat: 'all' })))).toBe('e');
    expect(upNext(q, 10)).toEqual(['a', 'b', 'c', 'd', 'e']);
  });

  it('shuffle mantém a faixa atual e cobre todas as faixas', () => {
    const q = setShuffle(createQueue(ctx, 2), true);
    expect(currentTrack(q)).toBe('c');
    expect([currentTrack(q), ...upNext(q)].sort()).toEqual([...ctx].sort());
    const off = setShuffle(q, false);
    expect(currentTrack(off)).toBe('c');
    expect(upNext(off)).toEqual(['d', 'e']);
  });

  it('reordena o "a seguir"', () => {
    const q = moveInUpcomingContext(createQueue(ctx, 0), 0, 3);
    expect(upNext(q)).toEqual(['c', 'd', 'e', 'b']);
  });
});
