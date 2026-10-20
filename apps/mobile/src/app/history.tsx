import { useMemo } from 'react';
import { SectionList, Text, View } from 'react-native';
import { router } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { localDay } from '@sonora/core';
import { useAsync } from '@/components/hooks';
import { TrackRow } from '@/components/TrackRow';
import { Empty, formatMinutes, IconButton } from '@/components/ui';
import { playsBetween } from '@/db/local';
import { useTracks } from '@/library/useTracks';
import { playTracks } from '@/player/engine';
import { colors, space, type } from '@/theme';

/** Histórico completo, agrupado por dia local, com total ouvido por dia. */
export default function History() {
  const insets = useSafeAreaInsets();
  const { data: events } = useAsync(() => playsBetween(0, Date.now() + 1), []);
  const recent = useMemo(() => (events ?? []).slice(-1000).reverse(), [events]);
  const { tracks } = useTracks([...new Set(recent.map((e) => e.trackId))]);
  const byId = useMemo(() => new Map(tracks.map((t) => [t.id, t])), [tracks]);

  const sections = useMemo(() => {
    const m = new Map<string, { ms: number; data: typeof recent }>();
    for (const e of recent) {
      const d = localDay(e.startedAt, e.tzOffsetMin);
      const s = m.get(d) ?? { ms: 0, data: [] };
      s.ms += e.listenedMs;
      s.data.push(e);
      m.set(d, s);
    }
    return [...m.entries()].map(([day, s]) => ({ day, ms: s.ms, data: s.data }));
  }, [recent]);

  return (
    <View style={{ flex: 1, backgroundColor: colors.bg, paddingTop: insets.top }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', paddingHorizontal: space.sm }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <Text style={type.title}>Histórico</Text>
      </View>
      <SectionList
        sections={sections}
        keyExtractor={(e) => e.id}
        contentContainerStyle={{ paddingHorizontal: space.lg, paddingBottom: 120 }}
        ListEmptyComponent={<Empty icon="time-outline" title="Nada tocado ainda" />}
        renderSectionHeader={({ section }) => (
          <View style={{ backgroundColor: colors.bg, paddingVertical: space.sm, flexDirection: 'row', justifyContent: 'space-between' }}>
            <Text style={type.subtitle}>{new Date(`${section.day}T12:00:00`).toLocaleDateString('pt-BR', { weekday: 'long', day: '2-digit', month: 'long' })}</Text>
            <Text style={type.caption}>{formatMinutes(section.ms)}</Text>
          </View>
        )}
        renderItem={({ item }) => {
          const t = byId.get(item.trackId);
          if (!t) return null;
          return <TrackRow track={t} onPress={() => void playTracks([t], 0, { type: 'library' }, 'Histórico')} />;
        }}
      />
    </View>
  );
}
