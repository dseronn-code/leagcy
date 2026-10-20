import { ScrollView, Text, View } from 'react-native';
import { router, useLocalSearchParams } from 'expo-router';
import { pickSource, qualityLabel } from '@sonora/core';
import { api } from '@/api/client';
import { CollectionHeader, PlayButtons } from '@/components/CollectionHeader';
import { useAsync } from '@/components/hooks';
import { TrackRow } from '@/components/TrackRow';
import { Empty, formatMinutes, IconButton, Loading } from '@/components/ui';
import { download } from '@/downloads/manager';
import { toggleSaveAlbum, useLibrary } from '@/library/store';
import { deviceCaps } from '@/player/caps';
import { playTracks } from '@/player/engine';
import { colors, space, type } from '@/theme';

export default function AlbumScreen() {
  const { id } = useLocalSearchParams<{ id: string }>();
  const { data, loading } = useAsync(() => api.album(id), [id]);
  const saved = useLibrary((s) => s.savedAlbums.some((a) => a.id === id));
  if (loading) return <Loading />;
  if (!data) return <Empty icon="disc-outline" title="Álbum indisponível" />;
  const { album, tracks } = data;
  const total = tracks.reduce((s, t) => s + t.durationMs, 0);
  const best = tracks[0] && pickSource(tracks[0].sources, 'max', deviceCaps);
  const ctx = { type: 'album' as const, id: album.id };
  const discs = [...new Set(tracks.map((t) => t.discNumber ?? 1))];

  return (
    <ScrollView style={{ backgroundColor: colors.bg }} contentContainerStyle={{ paddingBottom: 120 }}>
      <CollectionHeader
        artwork={album.artworkUrl}
        title={album.title}
        subtitle={album.artists.map((a) => a.name).join(', ')}
        meta={`${{ album: 'Álbum', single: 'Single', ep: 'EP', compilation: 'Coletânea', live: 'Ao vivo' }[album.type]}${album.year ? ` · ${album.year}` : ''} · ${tracks.length} músicas · ${formatMinutes(total)}${best ? ` · ${qualityLabel(best)}` : ''}`}
        actions={
          <>
            <IconButton name={saved ? 'checkmark-circle' : 'add-circle-outline'} color={saved ? colors.accent : colors.text} label={saved ? 'Remover da biblioteca' : 'Salvar na biblioteca'} onPress={() => void toggleSaveAlbum({ id: album.id, title: album.title, artist: album.artists[0]?.name ?? '', artworkUrl: album.artworkUrl })} />
            <IconButton name="arrow-down-circle-outline" label="Baixar álbum" onPress={() => download(tracks)} />
            <IconButton name="person-outline" label="Ver artista" onPress={() => album.artists[0] && router.push(`/artist/${encodeURIComponent(album.artists[0].id)}`)} />
            <PlayButtons onPlay={() => void playTracks(tracks, 0, ctx, album.title, { shuffle: false })} onShuffle={() => void playTracks(tracks, 0, ctx, album.title, { shuffle: true })} />
          </>
        }
      />
      <View style={{ paddingHorizontal: space.lg }}>
        {discs.map((d) => (
          <View key={d}>
            {discs.length > 1 && <Text style={[type.micro, { marginTop: space.lg }]}>DISCO {d}</Text>}
            {tracks.map((t, i) => ((t.discNumber ?? 1) === d ? <TrackRow key={t.id} track={t} index={(t.trackNumber ?? i + 1) - 1} showArtwork={false} onPress={() => void playTracks(tracks, i, ctx, album.title)} /> : null))}
          </View>
        ))}
        {album.label && <Text style={[type.micro, { marginTop: space.lg }]}>{album.releaseDate ?? album.year} · ℗ {album.label}</Text>}
      </View>
    </ScrollView>
  );
}
