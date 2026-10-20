import { useState } from 'react';
import { Alert, Pressable, StyleSheet, Text, useWindowDimensions, View, type GestureResponderEvent } from 'react-native';
import { router } from 'expo-router';
import { LinearGradient } from 'expo-linear-gradient';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { qualityLabel } from '@sonora/core';
import { openTrackSheet } from '@/components/TrackSheet';
import { Artwork, IconButton, QualityBadge, formatDuration } from '@/components/ui';
import { toggleLike, useLibrary } from '@/library/store';
import { seek, setRate, setSleepTimer, skipNext, skipPrevious, togglePlay, toggleRepeat, toggleShuffle, usePlayer } from '@/player/engine';
import { colors, space, type } from '@/theme';

const RATES = [0.5, 0.75, 1, 1.25, 1.5, 2];

export default function Player() {
  const insets = useSafeAreaInsets();
  const { width } = useWindowDimensions();
  const p = usePlayer();
  const liked = useLibrary((s) => (p.current ? s.likes.has(p.current.id) : false));
  const [scrub, setScrub] = useState<number | null>(null);
  const [barWidth, setBarWidth] = useState(1);
  const t = p.current;

  if (!t) {
    router.back();
    return null;
  }

  const pos = scrub ?? p.positionSec;
  const dur = p.durationSec || t.durationMs / 1000;
  const fraction = (e: GestureResponderEvent) => Math.max(0, Math.min(1, e.nativeEvent.locationX / barWidth));

  const sleep = () =>
    Alert.alert('Timer de sono', p.sleepAt ? `Pausa às ${new Date(p.sleepAt).toLocaleTimeString('pt-BR', { hour: '2-digit', minute: '2-digit' })}` : undefined, [
      ...[15, 30, 45, 60].map((m) => ({ text: `${m} min`, onPress: () => setSleepTimer(m) })),
      { text: 'Desligar', onPress: () => setSleepTimer(null) },
      { text: 'Cancelar', style: 'cancel' as const },
    ]);

  return (
    <LinearGradient colors={['#2A1E66', colors.bg]} style={[styles.root, { paddingTop: insets.top + space.sm, paddingBottom: insets.bottom + space.lg }]}>
      <View style={styles.top}>
        <IconButton name="chevron-down" label="Fechar player" onPress={() => router.back()} />
        <View style={{ alignItems: 'center', flex: 1 }}>
          <Text style={type.micro}>TOCANDO DE</Text>
          <Text style={[type.caption, { color: colors.text }]} numberOfLines={1}>
            {p.contextTitle ?? 'Fila'}
          </Text>
        </View>
        <IconButton name="ellipsis-horizontal" label="Mais opções" onPress={() => openTrackSheet(t)} />
      </View>

      <View style={{ alignItems: 'center', marginTop: space.xl }}>
        <Artwork uri={t.artworkUrl} size={width - space.xl * 2} />
      </View>

      <View style={styles.titleRow}>
        <View style={{ flex: 1 }}>
          <Text style={type.title} numberOfLines={1}>
            {t.title}
          </Text>
          <Pressable onPress={() => (router.back(), router.push(`/artist/${encodeURIComponent(t.artists[0]!.id)}`))}>
            <Text style={[type.body, { color: colors.textDim }]} numberOfLines={1}>
              {t.artists.map((a) => a.name).join(', ')}
            </Text>
          </Pressable>
        </View>
        <IconButton name={liked ? 'heart' : 'heart-outline'} size={28} color={liked ? colors.accent : colors.text} label={liked ? 'Descurtir' : 'Curtir'} onPress={() => void toggleLike(t)} />
      </View>

      {/* Barra de progresso com toque/arraste para seek */}
      <View
        style={styles.barHit}
        onLayout={(e) => setBarWidth(e.nativeEvent.layout.width)}
        onStartShouldSetResponder={() => true}
        onResponderGrant={(e) => setScrub(fraction(e) * dur)}
        onResponderMove={(e) => setScrub(fraction(e) * dur)}
        onResponderRelease={(e) => {
          seek(fraction(e) * dur);
          setScrub(null);
        }}
        accessible
        accessibilityRole="adjustable"
        accessibilityLabel="Posição da música"
        accessibilityValue={{ min: 0, max: Math.round(dur), now: Math.round(pos), text: `${formatDuration(pos * 1000)} de ${formatDuration(dur * 1000)}` }}
        accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}
        onAccessibilityAction={(e) => seek(pos + (e.nativeEvent.actionName === 'increment' ? 10 : -10))}
      >
        <View style={styles.barBg}>
          <View style={[styles.barFill, { width: `${dur ? (pos / dur) * 100 : 0}%` }]} />
        </View>
      </View>
      <View style={styles.times}>
        <Text style={type.micro}>{formatDuration(pos * 1000)}</Text>
        <QualityBadge source={p.source} />
        <Text style={type.micro}>-{formatDuration(Math.max(0, dur - pos) * 1000)}</Text>
      </View>

      <View style={styles.controls}>
        <IconButton name="shuffle" color={p.queue?.shuffle ? colors.accent : colors.text} label={`Aleatório ${p.queue?.shuffle ? 'ligado' : 'desligado'}`} onPress={() => void toggleShuffle()} />
        <IconButton name="play-skip-back" size={34} label="Anterior" onPress={() => void skipPrevious()} />
        <Pressable style={styles.play} onPress={togglePlay} accessibilityRole="button" accessibilityLabel={p.playing ? 'Pausar' : 'Tocar'}>
          <IconButton name={p.playing ? 'pause' : 'play'} size={36} color={colors.bg} label={p.playing ? 'Pausar' : 'Tocar'} onPress={togglePlay} />
        </Pressable>
        <IconButton name="play-skip-forward" size={34} label="Próxima" onPress={() => void skipNext()} />
        <View>
          <IconButton name="repeat" color={p.queue?.repeat !== 'off' ? colors.accent : colors.text} label={`Repetir: ${p.queue?.repeat}`} onPress={toggleRepeat} />
          {p.queue?.repeat === 'one' && <Text style={styles.one}>1</Text>}
        </View>
      </View>

      <View style={styles.bottom}>
        <Pressable onPress={() => setRate(RATES[(RATES.indexOf(p.rate) + 1) % RATES.length]!)} accessibilityLabel={`Velocidade ${p.rate}x`} style={styles.rate}>
          <Text style={[type.caption, { color: p.rate !== 1 ? colors.accent : colors.text, fontWeight: '700' }]}>{p.rate}x</Text>
        </Pressable>
        <IconButton name="moon-outline" color={p.sleepAt ? colors.accent : colors.text} label="Timer de sono" onPress={sleep} />
        <IconButton name="chatbox-ellipses-outline" label="Letra" onPress={() => router.push('/lyrics')} />
        <IconButton name="options-outline" label="Equalizador" onPress={() => router.push('/equalizer')} />
        <IconButton name="list" label="Fila" onPress={() => router.push('/queue')} />
      </View>
      {p.source && <Text style={[type.micro, { textAlign: 'center', marginTop: space.sm }]}>{qualityLabel(p.source)} · sem compressão de dinâmica</Text>}
    </LinearGradient>
  );
}

const styles = StyleSheet.create({
  root: { flex: 1, paddingHorizontal: space.xl },
  top: { flexDirection: 'row', alignItems: 'center' },
  titleRow: { flexDirection: 'row', alignItems: 'center', marginTop: space.xl },
  barHit: { paddingVertical: space.md, marginTop: space.md },
  barBg: { height: 4, borderRadius: 2, backgroundColor: 'rgba(255,255,255,0.2)' },
  barFill: { height: 4, borderRadius: 2, backgroundColor: colors.text },
  times: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center' },
  controls: { flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between', marginTop: space.lg },
  play: { width: 72, height: 72, borderRadius: 36, backgroundColor: colors.text, alignItems: 'center', justifyContent: 'center' },
  one: { position: 'absolute', right: 8, top: 6, fontSize: 9, color: colors.accent, fontWeight: '800' },
  bottom: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginTop: 'auto' },
  rate: { minWidth: 44, minHeight: 44, alignItems: 'center', justifyContent: 'center' },
});
