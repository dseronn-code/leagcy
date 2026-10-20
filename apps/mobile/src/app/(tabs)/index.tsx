import { Pressable, Text, View } from 'react-native';
import { router } from 'expo-router';
import { api, type Mix } from '@/api/client';
import { useAsync } from '@/components/hooks';
import { Empty, HRow, IconButton, Loading, MediaCard, Screen, SectionHeader } from '@/components/ui';
import { useLibrary } from '@/library/store';
import { playTracks } from '@/player/engine';
import { colors, radius, space, type } from '@/theme';
import { cacheMix } from '@/library/mixCache';

function greeting() {
  const h = new Date().getHours();
  return h < 12 ? 'Bom dia' : h < 18 ? 'Boa tarde' : 'Boa noite';
}

export default function Home() {
  const { data, loading, error, reload } = useAsync(() => api.home(), []);
  const playlists = useLibrary((s) => s.playlists.slice(0, 6));

  const openMix = (m: Mix) => {
    cacheMix(m);
    router.push(`/mix/${encodeURIComponent(m.id)}`);
  };

  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center', justifyContent: 'space-between' }}>
        <Text style={type.hero}>{greeting()}</Text>
        <View style={{ flexDirection: 'row' }}>
          <IconButton name="time-outline" label="Histórico" onPress={() => router.push('/history')} />
          <IconButton name="settings-outline" label="Configurações" onPress={() => router.push('/settings')} />
        </View>
      </View>

      {playlists.length > 0 && (
        <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: space.sm, marginTop: space.lg }}>
          {playlists.map((p) => (
            <Pressable key={p.id} onPress={() => router.push(`/playlist/${p.id}`)} style={{ width: '48.5%', backgroundColor: colors.surface, borderRadius: radius.sm, padding: space.md, minHeight: 52, justifyContent: 'center' }}>
              <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
                {p.name}
              </Text>
            </Pressable>
          ))}
        </View>
      )}

      {loading && <Loading />}
      {error && <Empty icon="cloud-offline-outline" title="Sem conexão" body="Suas músicas baixadas continuam disponíveis na Biblioteca." />}
      {error && <Text style={[type.caption, { color: colors.accent, textAlign: 'center' }]} onPress={reload}>Tentar novamente</Text>}

      {data?.sections.map((s) => (
        <View key={s.id}>
          {(s.mixes?.length || s.tracks?.length) ? <SectionHeader title={s.title} /> : null}
          <HRow>
            {s.mixes?.filter((m) => m.tracks.length).map((m) => (
              <MediaCard key={m.id} title={m.title} subtitle={m.subtitle} uri={m.tracks[0]?.artworkUrl} onPress={() => openMix(m)} />
            ))}
            {s.tracks?.map((t, i) => (
              <MediaCard key={t.id} title={t.title} subtitle={t.artists[0]?.name} uri={t.artworkUrl} onPress={() => void playTracks(s.tracks!, i, { type: 'library' }, s.title)} />
            ))}
          </HRow>
        </View>
      ))}
    </Screen>
  );
}
