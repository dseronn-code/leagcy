import type { AudioSource, PlayEvent, Track } from '../src';

let n = 0;
export function ev(p: Partial<PlayEvent> & { startedAt: number }): PlayEvent {
  return {
    id: `e${n++}`,
    trackId: 't1',
    artistIds: ['a1'],
    genres: ['rock'],
    listenedMs: 180_000,
    trackDurationMs: 200_000,
    skipped: false,
    source: { type: 'library' },
    deviceId: 'd1',
    tzOffsetMin: 0,
    ...p,
  };
}

export function src(p: Partial<AudioSource>): AudioSource {
  return { url: 'x', codec: 'flac', bitrate: 1000, sampleRateHz: 44_100, bitDepth: 16, channels: 2, ...p };
}

export function track(id: string, artist: string, p: Partial<Track> = {}): Track {
  return {
    id,
    provider: 'test',
    title: id,
    artists: [{ id: artist, name: artist }],
    albumId: 'al',
    albumTitle: 'al',
    durationMs: 200_000,
    genres: ['rock'],
    sources: [src({})],
    ...p,
  };
}
