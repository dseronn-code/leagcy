import { useMemo, useState } from 'react';
import { Alert, Pressable, ScrollView, StyleSheet, Text, View } from 'react-native';
import { router } from 'expo-router';
import { Ionicons } from '@expo/vector-icons';
import { Artwork, Chip, Empty, IconButton, Screen } from '@/components/ui';
import { useDownloads } from '@/downloads/manager';
import { createPlaylist, useLibrary } from '@/library/store';
import { colors, radius, space, type } from '@/theme';

type Filter = 'playlists' | 'albums' | 'artists';
type Sort = 'recent' | 'alpha';

function Row({ title, subtitle, uri, icon, round, onPress }: { title: string; subtitle: string; uri?: string; icon?: keyof typeof Ionicons.glyphMap; round?: boolean; onPress: () => void }) {
  return (
    <Pressable style={({ pressed }) => [styles.row, pressed && { opacity: 0.7 }]} onPress={onPress} accessibilityRole="button" accessibilityLabel={`${title}, ${subtitle}`}>
      {icon ? (
        <View style={[styles.iconTile, round && { borderRadius: 32 }]}>
          <Ionicons name={icon} size={26} color={colors.text} />
        </View>
      ) : (
        <Artwork uri={uri} size={64} round={round} />
      )}
      <View style={{ flex: 1 }}>
        <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
          {title}
        </Text>
        <Text style={type.caption} numberOfLines={1}>
          {subtitle}
        </Text>
      </View>
    </Pressable>
  );
}

export default function Library() {
  const [filter, setFilter] = useState<Filter | null>(null);
  const [sort, setSort] = useState<Sort>('recent');
  const { playlists, savedAlbums, followedArtists, likes } = useLibrary();
  const downloads = useDownloads((s) => s.files.size);

  const sorted = useMemo(() => {
    const by = <T,>(arr: T[], name: (t: T) => string) => (sort === 'alpha' ? [...arr].sort((a, b) => name(a).localeCompare(name(b), 'pt-BR')) : arr);
    return { playlists: by(playlists, (p) => p.name), albums: by(savedAlbums, (a) => a.title), artists: by(followedArtists, (a) => a.name) };
  }, [playlists, savedAlbums, followedArtists, sort]);

  const newPlaylist = () =>
    Alert.alert('Nova playlist', 'Escolha o tipo', [
      { text: 'Playlist', onPress: () => void createPlaylist(`Minha playlist ${playlists.length + 1}`).then((id) => router.push(`/playlist/${id}`)) },
      { text: 'Colaborativa', onPress: () => void createPlaylist('Playlist colaborativa', { kind: 'collaborative' }).then((id) => router.push(`/playlist/${id}`)) },
      {
        text: 'Inteligente',
        onPress: () =>
          void createPlaylist('Hi-Res que eu amo', { kind: 'smart', smartRules: { match: 'all', rules: [{ field: 'liked', op: 'is', value: true }, { field: 'quality', op: 'is', value: 'hires' }], sort: 'playCount' } }).then((id) =>
            router.push(`/playlist/${id}`),
          ),
      },
      { text: 'Cancelar', style: 'cancel' },
    ]);

  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}>
        <Text style={type.hero}>Biblioteca</Text>
        <View style={{ flexDirection: 'row' }}>
          <IconButton name={sort === 'alpha' ? 'text-outline' : 'time-outline'} label={sort === 'alpha' ? 'Ordenado A-Z' : 'Ordenado por recentes'} onPress={() => setSort(sort === 'alpha' ? 'recent' : 'alpha')} />
          <IconButton name="add" label="Criar playlist" onPress={newPlaylist} />
        </View>
      </View>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ marginVertical: space.md }}>
        {(['playlists', 'albums', 'artists'] as const).map((f) => (
          <Chip key={f} label={{ playlists: 'Playlists', albums: 'Álbuns', artists: 'Artistas' }[f]} active={filter === f} onPress={() => setFilter(filter === f ? null : f)} />
        ))}
      </ScrollView>

      {(!filter || filter === 'playlists') && (
        <>
          <Row title="Músicas curtidas" subtitle={`${likes.size} músicas`} icon="heart" onPress={() => router.push('/playlist/liked')} />
          <Row title="Baixadas" subtitle={`${downloads} músicas offline`} icon="arrow-down-circle" onPress={() => router.push('/downloads')} />
          <Row title="Importar do Spotify" subtitle="Cole o link de uma playlist e ela fica sincronizada" icon="sync-circle" onPress={() => router.push('/spotify')} />
          {sorted.playlists.map((p) => (
            <Row
              key={p.id}
              title={p.name}
              subtitle={`${{ user: 'Playlist', collaborative: 'Colaborativa', smart: 'Inteligente', algorithmic: 'Mix', spotify: 'Do Spotify' }[p.kind]} · ${p.kind === 'smart' ? 'automática' : `${p.entries.length} músicas`}`}
              icon={p.artworkUrl ? undefined : p.kind === 'collaborative' ? 'people' : p.kind === 'smart' ? 'sparkles' : p.kind === 'spotify' ? 'sync-circle' : 'musical-notes'}
              uri={p.artworkUrl}
              onPress={() => router.push(`/playlist/${p.id}`)}
            />
          ))}
        </>
      )}
      {(!filter || filter === 'albums') &&
        sorted.albums.map((a) => <Row key={a.id} title={a.title} subtitle={`Álbum · ${a.artist}`} uri={a.artworkUrl} onPress={() => router.push(`/album/${encodeURIComponent(a.id)}`)} />)}
      {(!filter || filter === 'artists') &&
        sorted.artists.map((a) => <Row key={a.id} title={a.name} subtitle="Artista" uri={a.imageUrl} round onPress={() => router.push(`/artist/${encodeURIComponent(a.id)}`)} />)}
      {filter && !{ playlists: playlists.length, albums: savedAlbums.length, artists: followedArtists.length }[filter] && <Empty icon="albums-outline" title="Nada por aqui ainda" />}
    </Screen>
  );
}

const styles = StyleSheet.create({
  row: { flexDirection: 'row', alignItems: 'center', gap: space.md, paddingVertical: space.sm },
  iconTile: { width: 64, height: 64, borderRadius: radius.sm, backgroundColor: colors.accentSoft, alignItems: 'center', justifyContent: 'center' },
});
