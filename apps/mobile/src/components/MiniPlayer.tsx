import { Pressable, StyleSheet, Text, View } from 'react-native';
import { router } from 'expo-router';
import { toggleLike, useLibrary } from '@/library/store';
import { skipNext, togglePlay, usePlayer } from '@/player/engine';
import { colors, radius, space, type } from '@/theme';
import { Artwork, IconButton, QualityBadge } from './ui';

export function MiniPlayer() {
  const current = usePlayer((s) => s.current);
  const playing = usePlayer((s) => s.playing);
  const source = usePlayer((s) => s.source);
  const progress = usePlayer((s) => (s.durationSec ? s.positionSec / s.durationSec : 0));
  const liked = useLibrary((s) => (current ? s.likes.has(current.id) : false));
  if (!current) return null;

  return (
    <Pressable style={styles.wrap} onPress={() => router.push('/player')} accessibilityRole="button" accessibilityLabel={`Tocando ${current.title}. Abrir player.`}>
      <View style={styles.row}>
        <Artwork uri={current.artworkUrl} size={40} />
        <View style={{ flex: 1, marginHorizontal: space.md }}>
          <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
            {current.title}
          </Text>
          <View style={{ flexDirection: 'row', gap: 6, alignItems: 'center' }}>
            <QualityBadge source={source} compact />
            <Text style={type.caption} numberOfLines={1}>
              {current.artists.map((a) => a.name).join(', ')}
            </Text>
          </View>
        </View>
        <IconButton name={liked ? 'heart' : 'heart-outline'} color={liked ? colors.accent : colors.text} label="Curtir" onPress={() => void toggleLike(current)} />
        <IconButton name={playing ? 'pause' : 'play'} size={28} label={playing ? 'Pausar' : 'Tocar'} onPress={togglePlay} />
        <IconButton name="play-skip-forward" label="Próxima" onPress={() => void skipNext()} />
      </View>
      <View style={styles.track}>
        <View style={[styles.fill, { width: `${Math.min(100, progress * 100)}%` }]} />
      </View>
    </Pressable>
  );
}

const styles = StyleSheet.create({
  wrap: { marginHorizontal: space.sm, marginBottom: space.xs, backgroundColor: colors.surfaceHigh, borderRadius: radius.md, overflow: 'hidden' },
  row: { flexDirection: 'row', alignItems: 'center', padding: space.sm },
  track: { height: 2, backgroundColor: colors.border },
  fill: { height: 2, backgroundColor: colors.text },
});
