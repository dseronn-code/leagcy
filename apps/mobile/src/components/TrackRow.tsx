import { memo } from 'react';
import { Pressable, StyleSheet, Text, View } from 'react-native';
import { Ionicons } from '@expo/vector-icons';
import { pickSource, type Track } from '@sonora/core';
import { useDownloads } from '@/downloads/manager';
import { useLibrary } from '@/library/store';
import { usePlayer } from '@/player/engine';
import { deviceCaps } from '@/player/caps';
import { colors, space, type } from '@/theme';
import { openTrackSheet } from './TrackSheet';
import { Artwork, IconButton, QualityBadge, formatDuration } from './ui';

interface Props {
  track: Track;
  index?: number;
  onPress: () => void;
  showArtwork?: boolean;
  /** Elemento à direita (ex.: alça de arrastar). */
  trailing?: React.ReactNode;
}

export const TrackRow = memo(function TrackRow({ track, index, onPress, showArtwork = true, trailing }: Props) {
  const playing = usePlayer((s) => s.current?.id === track.id);
  const liked = useLibrary((s) => s.likes.has(track.id));
  const downloaded = useDownloads((s) => s.files.has(track.id));
  const best = pickSource(track.sources, 'max', deviceCaps);
  const artists = track.artists.map((a) => a.name).join(', ');

  return (
    <Pressable
      onPress={onPress}
      onLongPress={() => openTrackSheet(track)}
      style={({ pressed }) => [styles.row, pressed && { backgroundColor: colors.surface }]}
      accessibilityRole="button"
      accessibilityLabel={`${track.title}, ${artists}${playing ? ', tocando agora' : ''}`}
      accessibilityHint="Toque para tocar. Toque longo para mais opções."
    >
      {showArtwork ? (
        <Artwork uri={track.artworkUrl} size={48} />
      ) : (
        <Text style={[type.caption, styles.index]}>{index !== undefined ? index + 1 : ''}</Text>
      )}
      <View style={{ flex: 1, marginLeft: space.md }}>
        <Text style={[type.body, { fontWeight: '600', color: playing ? colors.accent : colors.text }]} numberOfLines={1}>
          {track.title}
        </Text>
        <View style={styles.meta}>
          {track.explicit && <Text style={styles.explicit}>E</Text>}
          {downloaded && <Ionicons name="arrow-down-circle" size={14} color={colors.success} />}
          <QualityBadge source={best} compact />
          <Text style={[type.caption, { flex: 1 }]} numberOfLines={1}>
            {artists} · {formatDuration(track.durationMs)}
          </Text>
        </View>
      </View>
      {liked && <Ionicons name="heart" size={16} color={colors.accent} style={{ marginHorizontal: space.xs }} />}
      {trailing ?? <IconButton name="ellipsis-vertical" size={18} color={colors.textDim} label="Mais opções" onPress={() => openTrackSheet(track)} />}
    </Pressable>
  );
});

const styles = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', paddingVertical: space.sm, minHeight: 64 },
  index: { width: 28, textAlign: 'center' },
  meta: { flexDirection: 'row', alignItems: 'center', gap: 6, marginTop: 2 },
  explicit: { fontSize: 10, color: colors.bg, backgroundColor: colors.textDim, paddingHorizontal: 4, borderRadius: 2, fontWeight: '700' },
});
