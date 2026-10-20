import { ScrollView, Text, View } from 'react-native';
import { router, useLocalSearchParams } from 'expo-router';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { LinearGradient } from 'expo-linear-gradient';
import type { Album } from '@sonora/core';
import { api } from '@/api/client';
import { PlayButtons } from '@/components/CollectionHeader';
import { useAsync } from '@/components/hooks';
import { TrackRow } from '@/components/TrackRow';
import { Artwork, Button, Empty, HRow, IconButton, Loading, MediaCard, SectionHeader } from '@/components/ui';
import { toggleFollowArtist, useLibrary } from '@/library/store';
import { playTracks, startRadio } from '@/player/engine';
import { colors, space, type } from '@/theme';

export default function ArtistScreen() {
  const insets = useSafeAreaInsets();
  const { id } = useLocalSearchParams<{ id: string }>();
  const { data, loading } = useAsync(() => api.artist(id), [id]);
  const following = useLibrary((s) => s.followedArtists.some((a) => a.id === id));
  if (loading) return <Loading />;
  if (!data) return <Empty icon="person-outline" title="Artista indisponível" />;
  const { artist, topTracks, albums, singles, compilations, related } = data;
  const ctx = { type: 'artist' as const, id: artist.id };

  const shelf = (title: string, items: Album[]) =>
    items.length > 0 && (
      <>
        <SectionHeader title={title} />
        <HRow>
          {items.map((a) => (
            <MediaCard key={a.id} title={a.title} subtitle={[a.year, a.bestQuality && (a.bestQuality.bitDepth ?? 16) > 16 ? 'Hi-Res' : undefined].filter(Boolean).join(' · ')} uri={a.artworkUrl} onPress={() => router.push(`/album/${encodeURIComponent(a.id)}`)} />
          ))}
        </HRow>
      </>
    );

  return (
    <ScrollView style={{ backgroundColor: colors.bg }} contentContainerStyle={{ paddingBottom: 120 }}>
      <LinearGradient colors={['#2A1E66', colors.bg]} style={{ paddingTop: insets.top, paddingHorizontal: space.lg }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <View style={{ alignItems: 'center' }}>
          <Artwork uri={artist.imageUrl} size={180} round />
          <Text style={[type.hero, { marginTop: space.lg, textAlign: 'center' }]}>{artist.name}</Text>
          {artist.monthlyListeners !== undefined && <Text style={type.caption}>{artist.monthlyListeners.toLocaleString('pt-BR')} ouvintes mensais</Text>}
        </View>
        <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: space.lg, gap: space.sm }}>
          <Button variant="secondary" title={following ? 'Seguindo' : 'Seguir'} onPress={() => void toggleFollowArtist({ id: artist.id, name: artist.name, imageUrl: artist.imageUrl })} />
          <IconButton name="radio-outline" label="Rádio do artista" onPress={() => topTracks[0] && void startRadio(topTracks[0])} />
          <PlayButtons onPlay={() => void playTracks(topTracks, 0, ctx, artist.name, { shuffle: false })} onShuffle={() => void playTracks(topTracks, 0, ctx, artist.name, { shuffle: true })} />
        </View>
      </LinearGradient>
      <View style={{ paddingHorizontal: space.lg }}>
        {topTracks.length > 0 && <SectionHeader title="Mais ouvidas" />}
        {topTracks.slice(0, 10).map((t, i) => (
          <TrackRow key={t.id} track={t} onPress={() => void playTracks(topTracks, i, ctx, artist.name)} />
        ))}
        {shelf('Discografia', albums)}
        {shelf('Singles e EPs', singles)}
        {shelf('Coletâneas', compilations)}
        {related.length > 0 && (
          <>
            <SectionHeader title="Artistas relacionados" />
            <HRow>
              {related.map((a) => (
                <MediaCard key={a.id} title={a.name} uri={a.imageUrl} round size={120} onPress={() => router.push(`/artist/${encodeURIComponent(a.id)}`)} />
              ))}
            </HRow>
          </>
        )}
        {artist.bio && (
          <>
            <SectionHeader title="Sobre" />
            <Text style={[type.body, { color: colors.textDim, lineHeight: 22 }]}>{artist.bio.replace(/<[^>]+>/g, '')}</Text>
          </>
        )}
      </View>
    </ScrollView>
  );
}
