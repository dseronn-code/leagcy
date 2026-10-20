import { useEffect, useState } from 'react';
import type { Track } from '@sonora/core';
import { api } from '@/api/client';
import { cachedTracks, cacheTracks } from '@/db/local';

/** Resolve IDs em faixas: primeiro o cache local (offline), depois a API para o que faltar. */
export function useTracks(ids: string[]): { tracks: Track[]; loading: boolean } {
  const key = ids.join(',');
  const [map, setMap] = useState<Map<string, Track>>(new Map());
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    let alive = true;
    (async () => {
      const local = await cachedTracks(ids);
      if (!alive) return;
      setMap(local);
      const missing = ids.filter((id) => !local.has(id));
      for (let i = 0; i < missing.length; i += 200) {
        try {
          const { tracks } = await api.tracks(missing.slice(i, i + 200));
          await cacheTracks(tracks);
          if (!alive) return;
          setMap((m) => new Map([...m, ...tracks.map((t) => [t.id, t] as const)]));
        } catch {
          break; // offline
        }
      }
      if (alive) setLoading(false);
    })();
    return () => {
      alive = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key]);

  return { tracks: ids.map((id) => map.get(id)).filter((t): t is Track => !!t), loading };
}
