import { Modal, Pressable, ScrollView, Share, StyleSheet, Text, View } from 'react-native';
import { router } from 'expo-router';
import { Ionicons } from '@expo/vector-icons';
import { create } from 'zustand';
import type { Track } from '@sonora/core';
import { deleteDownload, download, useDownloads } from '@/downloads/manager';
import { addToPlaylist, createPlaylist, toggleLike, useLibrary } from '@/library/store';
import { addToQueue, playNext, startRadio } from '@/player/engine';
import { colors, radius, space, type } from '@/theme';
import { Artwork, type IconName } from './ui';

const useSheet = create<{ track: Track | null; mode: 'actions' | 'playlists' }>(() => ({ track: null, mode: 'actions' }));

export function openTrackSheet(track: Track) {
  useSheet.setState({ track, mode: 'actions' });
}

const close = () => useSheet.setState({ track: null });

function Action({ icon, label, onPress }: { icon: IconName; label: string; onPress: () => void }) {
  return (
    <Pressable style={({ pressed }) => [styles.action, pressed && { backgroundColor: colors.surfaceHigh }]} onPress={onPress} accessibilityRole="button">
      <Ionicons name={icon} size={22} color={colors.text} />
      <Text style={type.body}>{label}</Text>
    </Pressable>
  );
}

/** Menu de ações de faixa, montado uma vez no layout raiz. */
export function TrackSheet() {
  const { track, mode } = useSheet();
  const liked = useLibrary((s) => (track ? s.likes.has(track.id) : false));
  const playlists = useLibrary((s) => s.playlists.filter((p) => p.kind !== 'smart' && p.kind !== 'algorithmic'));
  const downloaded = useDownloads((s) => (track ? s.files.has(track.id) : false));
  if (!track) return null;

  const run = (fn: () => unknown) => () => {
    close();
    void fn();
  };

  return (
    <Modal transparent animationType="slide" visible onRequestClose={close}>
      <Pressable style={styles.backdrop} onPress={close} accessibilityLabel="Fechar" />
      <View style={styles.sheet}>
        <View style={styles.header}>
          <Artwork uri={track.artworkUrl} size={52} />
          <View style={{ flex: 1 }}>
            <Text style={type.subtitle} numberOfLines={1}>
              {track.title}
            </Text>
            <Text style={type.caption} numberOfLines={1}>
              {track.artists.map((a) => a.name).join(', ')} · {track.albumTitle}
            </Text>
          </View>
        </View>
        <ScrollView style={{ maxHeight: 460 }}>
          {mode === 'actions' ? (
            <>
              <Action icon={liked ? 'heart' : 'heart-outline'} label={liked ? 'Remover das curtidas' : 'Curtir'} onPress={run(() => toggleLike(track))} />
              <Action icon="play-skip-forward-outline" label="Tocar a seguir" onPress={run(() => playNext([track]))} />
              <Action icon="list-outline" label="Adicionar à fila" onPress={run(() => addToQueue([track]))} />
              <Action icon="add-circle-outline" label="Adicionar à playlist" onPress={() => useSheet.setState({ mode: 'playlists' })} />
              <Action icon="radio-outline" label="Ir para o rádio da música" onPress={run(() => startRadio(track))} />
              <Action
                icon={downloaded ? 'trash-outline' : 'arrow-down-circle-outline'}
                label={downloaded ? 'Remover download' : 'Baixar para ouvir offline'}
                onPress={run(() => (downloaded ? deleteDownload(track.id) : download([track])))}
              />
              <Action icon="disc-outline" label="Ver álbum" onPress={run(() => router.push(`/album/${encodeURIComponent(track.albumId)}`))} />
              {track.artists.map((a) => (
                <Action key={a.id} icon="person-outline" label={`Ver artista: ${a.name}`} onPress={run(() => router.push(`/artist/${encodeURIComponent(a.id)}`))} />
              ))}
              <Action icon="share-outline" label="Compartilhar" onPress={run(() => Share.share({ message: `${track.title} — ${track.artists[0]?.name} sonora://track/${track.id}` }))} />
            </>
          ) : (
            <>
              <Action
                icon="add"
                label="Nova playlist"
                onPress={run(async () => {
                  const id = await createPlaylist(`Minha playlist ${playlists.length + 1}`, { tracks: [track] });
                  router.push(`/playlist/${id}`);
                })}
              />
              {playlists.map((p) => (
                <Action key={p.id} icon={p.kind === 'collaborative' ? 'people-outline' : 'musical-notes-outline'} label={p.name} onPress={run(() => addToPlaylist(p.id, [track]))} />
              ))}
            </>
          )}
        </ScrollView>
      </View>
    </Modal>
  );
}

const styles = StyleSheet.create({
  backdrop: { flex: 1, backgroundColor: 'rgba(0,0,0,0.6)' },
  sheet: { backgroundColor: colors.surface, borderTopLeftRadius: radius.lg, borderTopRightRadius: radius.lg, paddingBottom: space.xxl, paddingTop: space.md },
  header: { flexDirection: 'row', gap: space.md, alignItems: 'center', paddingHorizontal: space.lg, paddingBottom: space.md, borderBottomWidth: StyleSheet.hairlineWidth, borderColor: colors.border },
  action: { flexDirection: 'row', alignItems: 'center', gap: space.lg, paddingHorizontal: space.lg, minHeight: 52 },
});
