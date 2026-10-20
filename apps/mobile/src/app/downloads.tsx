import { Alert, Text, View } from 'react-native';
import { router } from 'expo-router';
import { Paths } from 'expo-file-system';
import { TrackRow } from '@/components/TrackRow';
import { Button, Empty, IconButton, Screen } from '@/components/ui';
import { deleteAllDownloads, deleteDownload, useDownloads } from '@/downloads/manager';
import { useTracks } from '@/library/useTracks';
import { playTracks } from '@/player/engine';
import { colors, space, type } from '@/theme';

const gb = (b: number) => `${(b / 1024 ** 3).toFixed(2)} GB`;

export default function Downloads() {
  const { files, active, totalBytes } = useDownloads();
  const { tracks } = useTracks([...files.keys()]);
  const free = Paths.availableDiskSpace;

  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <Text style={type.title}>Baixadas</Text>
      </View>
      <Text style={[type.caption, { marginVertical: space.md }]}>
        {files.size} músicas · {gb(totalBytes)} usados · {gb(free)} livres{active.size ? ` · baixando ${active.size}` : ''}
      </Text>
      {tracks.length === 0 && <Empty icon="arrow-down-circle-outline" title="Nada baixado" body="Baixe álbuns, playlists ou músicas para ouvir sem internet — no codec original, sem perda." />}
      {tracks.map((t, i) => (
        <TrackRow key={t.id} track={t} onPress={() => void playTracks(tracks, i, { type: 'library' }, 'Baixadas')} trailing={<IconButton name="trash-outline" color={colors.textDim} label={`Apagar ${t.title}`} onPress={() => void deleteDownload(t.id)} />} />
      ))}
      {tracks.length > 0 && (
        <View style={{ marginTop: space.xl }}>
          <Button variant="secondary" title="Apagar todos os downloads" onPress={() => Alert.alert('Apagar downloads?', gb(totalBytes), [{ text: 'Cancelar', style: 'cancel' }, { text: 'Apagar', style: 'destructive', onPress: () => void deleteAllDownloads() }])} />
        </View>
      )}
    </Screen>
  );
}
