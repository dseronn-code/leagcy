import { ScrollView, View } from 'react-native';
import { useLocalSearchParams } from 'expo-router';
import { CollectionHeader, PlayButtons } from '@/components/CollectionHeader';
import { TrackRow } from '@/components/TrackRow';
import { Empty, IconButton } from '@/components/ui';
import { download } from '@/downloads/manager';
import { getMix } from '@/library/mixCache';
import { createPlaylist } from '@/library/store';
import { playTracks } from '@/player/engine';
import { colors, space } from '@/theme';

/** Discover Weekly / Daily Mix: gerados no servidor, podem ser salvos como playlist. */
export default function MixScreen() {
  const { id } = useLocalSearchParams<{ id: string }>();
  const mix = getMix(id);
  if (!mix) return <Empty icon="sparkles-outline" title="Mix expirado" body="Volte ao Início para atualizar seus mixes." />;
  const ctx = { type: 'playlist' as const, id: mix.id };
  return (
    <ScrollView style={{ backgroundColor: colors.bg }} contentContainerStyle={{ paddingBottom: 120 }}>
      <CollectionHeader
        artwork={mix.tracks[0]?.artworkUrl}
        title={mix.title}
        subtitle={mix.subtitle}
        meta={`Feito para você · ${mix.tracks.length} músicas`}
        actions={
          <>
            <IconButton name="add-circle-outline" label="Salvar como playlist" onPress={() => void createPlaylist(mix.title, { tracks: mix.tracks })} />
            <IconButton name="arrow-down-circle-outline" label="Baixar mix" onPress={() => download(mix.tracks)} />
            <PlayButtons onPlay={() => void playTracks(mix.tracks, 0, ctx, mix.title, { shuffle: false })} onShuffle={() => void playTracks(mix.tracks, 0, ctx, mix.title, { shuffle: true })} />
          </>
        }
      />
      <View style={{ paddingHorizontal: space.lg }}>
        {mix.tracks.map((t, i) => (
          <TrackRow key={t.id} track={t} onPress={() => void playTracks(mix.tracks, i, ctx, mix.title)} />
        ))}
      </View>
    </ScrollView>
  );
}
