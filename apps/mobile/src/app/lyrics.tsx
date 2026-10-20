import { useEffect, useMemo, useRef } from 'react';
import { Pressable, ScrollView, Text, View, type ScrollViewInstance } from 'react-native';
import { router } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { activeLineIndex, parseLrc } from '@sonora/core';
import { api } from '@/api/client';
import { useAsync } from '@/components/hooks';
import { Empty, IconButton, Loading } from '@/components/ui';
import { seek, usePlayer } from '@/player/engine';
import { colors, space, type } from '@/theme';

/** Letras sincronizadas com rolagem automática; tocar numa linha faz seek para ela. */
export default function Lyrics() {
  const insets = useSafeAreaInsets();
  const current = usePlayer((s) => s.current);
  const pos = usePlayer((s) => s.positionSec);
  const { data, loading } = useAsync(async () => (current ? api.lyrics(current.id) : null), [current?.id]);
  const lyrics = useMemo(() => (data?.synced ? parseLrc(data.synced) : data?.plain ? parseLrc(data.plain) : null), [data]);
  const active = lyrics?.synced ? activeLineIndex(lyrics.lines, pos * 1000 + 150) : -1;
  const scroll = useRef<ScrollViewInstance>(null);
  const offsets = useRef<number[]>([]);

  useEffect(() => {
    if (active >= 0) scroll.current?.scrollTo({ y: Math.max(0, (offsets.current[active] ?? 0) - 200), animated: true });
  }, [active]);

  return (
    <View style={{ flex: 1, backgroundColor: '#1B1240', paddingTop: insets.top }}>
      <View style={{ flexDirection: 'row', alignItems: 'center', paddingHorizontal: space.sm }}>
        <IconButton name="chevron-down" label="Fechar" onPress={() => router.back()} />
        <View style={{ flex: 1, alignItems: 'center' }}>
          <Text style={type.subtitle} numberOfLines={1}>
            {current?.title}
          </Text>
          <Text style={type.caption}>{current?.artists[0]?.name}</Text>
        </View>
        <View style={{ width: 44 }} />
      </View>
      {loading && <Loading />}
      {!loading && !lyrics && <Empty icon="chatbox-outline" title="Letra indisponível" body="Ainda não temos a letra desta música." />}
      {lyrics && (
        <ScrollView ref={scroll} contentContainerStyle={{ padding: space.xl, paddingBottom: 300 }}>
          {lyrics.lines.map((l, i) => (
            <Pressable
              key={`${l.timeMs}-${i}`}
              disabled={!lyrics.synced}
              onPress={() => seek(l.timeMs / 1000)}
              onLayout={(e) => (offsets.current[i] = e.nativeEvent.layout.y)}
              accessibilityRole={lyrics.synced ? 'button' : 'text'}
            >
              <Text
                style={{
                  fontSize: 26,
                  fontWeight: '800',
                  lineHeight: 34,
                  marginBottom: space.lg,
                  color: !lyrics.synced || i === active ? colors.text : i < active ? 'rgba(255,255,255,0.45)' : 'rgba(255,255,255,0.3)',
                }}
              >
                {l.text || '♪'}
              </Text>
            </Pressable>
          ))}
          {!lyrics.synced && <Text style={type.micro}>Letra não sincronizada</Text>}
          <Text style={[type.micro, { marginTop: space.xl }]}>Letras: LRCLIB</Text>
        </ScrollView>
      )}
    </View>
  );
}
