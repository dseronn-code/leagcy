import { useState } from 'react';
import { ScrollView, Text, View } from 'react-native';
import { router } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { upNext } from '@sonora/core';
import { ReorderableList } from '@/components/ReorderableList';
import { TrackRow } from '@/components/TrackRow';
import { IconButton, SectionHeader } from '@/components/ui';
import { jumpTo, playFromUserQueue, removeQueued, reorderQueue, setAutoplay, usePlayer } from '@/player/engine';
import { colors, space, type } from '@/theme';

const ROW = 64;

export default function Queue() {
  const insets = useSafeAreaInsets();
  const { queue, tracks, current, contextTitle, autoplay } = usePlayer();
  const [scrollEnabled, setScrollEnabled] = useState(true);
  if (!queue) return null;

  const user = queue.userQueue.map((id) => tracks[id]).filter((t) => !!t);
  // Próximas do contexto = upNext sem a fila do usuário.
  const contextUpcoming = upNext({ ...queue, userQueue: [] }, 200).map((id) => tracks[id]).filter((t) => !!t);
  const contextIndexOf = (offset: number) => queue.order[queue.cursor + 1 + offset]!;

  return (
    <View style={{ flex: 1, backgroundColor: colors.bg, paddingTop: insets.top }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', paddingHorizontal: space.sm }}>
        <IconButton name="chevron-down" label="Fechar" onPress={() => router.back()} />
        <Text style={[type.subtitle, { flex: 1, textAlign: 'center' }]}>Fila</Text>
        <IconButton name={autoplay ? 'infinite' : 'infinite-outline'} color={autoplay ? colors.accent : colors.textDim} label={`Reprodução automática ${autoplay ? 'ligada' : 'desligada'}`} onPress={() => setAutoplay(!autoplay)} />
      </View>
      <ScrollView scrollEnabled={scrollEnabled} contentContainerStyle={{ paddingHorizontal: space.lg, paddingBottom: 80 }}>
        <SectionHeader title="Tocando agora" />
        {current && <TrackRow track={current} onPress={() => router.back()} />}

        {user.length > 0 && (
          <>
            <SectionHeader title="Próximas na fila" />
            <ReorderableList
              items={user}
              keyOf={(t) => `u-${t.id}-${queue.userQueue.indexOf(t.id)}`}
              rowHeight={ROW}
              onDragStateChange={(d) => setScrollEnabled(!d)}
              onMove={(f, to) => void reorderQueue('user', f, to)}
              renderRow={(t, i, handle) => (
                <View style={{ flexDirection: 'row', alignItems: 'center' }}>
                  <View style={{ flex: 1 }}>
                    <TrackRow track={t} onPress={() => void playFromUserQueue(i)} trailing={<IconButton name="close" size={18} color={colors.textDim} label="Remover da fila" onPress={() => void removeQueued(i)} />} />
                  </View>
                  {handle}
                </View>
              )}
            />
          </>
        )}

        <SectionHeader title={`A seguir de: ${contextTitle ?? 'Fila'}`} />
        <ReorderableList
          items={contextUpcoming}
          keyOf={(t) => `c-${t.id}`}
          rowHeight={ROW}
          onDragStateChange={(d) => setScrollEnabled(!d)}
          onMove={(f, to) => void reorderQueue('context', f, to)}
          renderRow={(t, i, handle) => (
            <View style={{ flexDirection: 'row', alignItems: 'center' }}>
              <View style={{ flex: 1 }}>
                <TrackRow track={t} onPress={() => void jumpTo(contextIndexOf(i))} trailing={<View />} />
              </View>
              {handle}
            </View>
          )}
        />
        {autoplay && <Text style={[type.caption, { textAlign: 'center', marginTop: space.lg }]}>Quando a fila acabar, tocaremos músicas parecidas.</Text>}
      </ScrollView>
    </View>
  );
}
