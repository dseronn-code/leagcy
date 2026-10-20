import { useEffect, useState } from 'react';
import { ActivityIndicator, Alert, Pressable, StyleSheet, Text, TextInput, View } from 'react-native';
import { router } from 'expo-router';
import * as AuthSession from 'expo-auth-session';
import * as Clipboard from 'expo-clipboard';
import * as WebBrowser from 'expo-web-browser';
import { Ionicons } from '@expo/vector-icons';
import { parseSpotifyPlaylist } from '@sonora/core';
import { api, ApiError, type SpotifyImport } from '@/api/client';
import { useAsync } from '@/components/hooks';
import { Artwork, Button, Empty, IconButton, Loading, Screen, SectionHeader } from '@/components/ui';
import { syncNow } from '@/sync/engine';
import { colors, radius, space, type } from '@/theme';

WebBrowser.maybeCompleteAuthSession();

const discovery = { authorizationEndpoint: 'https://accounts.spotify.com/authorize', tokenEndpoint: 'https://accounts.spotify.com/api/token' };
// Cadastre exatamente este endereço em "Redirect URIs" no app do developer.spotify.com.
const redirectUri = AuthSession.makeRedirectUri({ scheme: 'sonora', path: 'spotify-callback' });

function ago(ms: number) {
  if (!ms) return 'nunca';
  const min = Math.round((Date.now() - ms) / 60_000);
  return min < 1 ? 'agora' : min < 60 ? `há ${min} min` : `há ${Math.round(min / 60)} h`;
}

/** Conecta a conta do Spotify e importa playlists pelo link; o servidor mantém tudo sincronizado. */
export default function SpotifyScreen() {
  const { data: status, reload: reloadStatus } = useAsync(() => api.spotifyStatus(), []);
  const { data: list, reload } = useAsync(async () => (status?.connected ? (await api.spotifyPlaylists()).playlists : []), [status?.connected]);
  const [link, setLink] = useState('');
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [request, response, promptAsync] = AuthSession.useAuthRequest(
    {
      clientId: status?.clientId ?? '',
      scopes: ['playlist-read-private', 'playlist-read-collaborative'],
      redirectUri,
      usePKCE: true,
      responseType: AuthSession.ResponseType.Code,
    },
    discovery,
  );

  useEffect(() => {
    if (response?.type !== 'success' || !request?.codeVerifier) return;
    api
      .spotifyConnect(response.params.code!, request.codeVerifier, redirectUri)
      .then(reloadStatus)
      .catch((e: Error) => setError(e.message));
  }, [response, request, reloadStatus]);

  const paste = async () => {
    const text = await Clipboard.getStringAsync();
    if (text) setLink(text.trim());
  };

  const importLink = async () => {
    if (!parseSpotifyPlaylist(link)) return setError('Cole o link de uma playlist (open.spotify.com/playlist/...)');
    setBusy(true);
    setError(null);
    try {
      const { playlist } = await api.spotifyImport(link);
      setLink('');
      await syncNow();
      reload();
      router.push(`/playlist/${playlist.localId}`);
    } catch (e) {
      setError(e instanceof ApiError ? e.message : 'Não foi possível importar agora');
    } finally {
      setBusy(false);
    }
  };

  const syncOne = async (p: SpotifyImport) => {
    await api.spotifySyncOne(p.spotifyId).catch(() => undefined);
    await syncNow();
    reload();
  };

  const stop = (p: SpotifyImport) =>
    Alert.alert('Parar de sincronizar?', `"${p.name}" continua na sua biblioteca como playlist comum, mas não acompanha mais o Spotify.`, [
      { text: 'Cancelar', style: 'cancel' },
      { text: 'Parar', style: 'destructive', onPress: () => void api.spotifyStop(p.spotifyId).then(() => syncNow()).then(reload) },
    ]);

  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <Text style={type.title}>Playlists do Spotify</Text>
      </View>

      {!status ? (
        <Loading />
      ) : !status.enabled ? (
        <Empty icon="construct-outline" title="Importação desligada" body="O servidor precisa do SPOTIFY_CLIENT_ID configurado (ver README)." />
      ) : !status.connected ? (
        <View style={{ gap: space.lg, marginTop: space.lg }}>
          <Text style={type.body}>Conecte sua conta para o Sonora ler suas playlists. Só lemos a lista de músicas — nada é alterado no Spotify.</Text>
          <Button title="Conectar com o Spotify" icon="link-outline" disabled={!request} onPress={() => void promptAsync()} />
        </View>
      ) : (
        <>
          <Text style={[type.caption, { marginTop: space.sm }]}>Conectado como {status.account?.displayName}</Text>
          <SectionHeader title="Importar playlist" />
          <View style={styles.inputRow}>
            <TextInput
              style={styles.input}
              placeholder="https://open.spotify.com/playlist/..."
              placeholderTextColor={colors.textMuted}
              value={link}
              onChangeText={setLink}
              autoCapitalize="none"
              autoCorrect={false}
              onSubmitEditing={() => void importLink()}
              accessibilityLabel="Link da playlist do Spotify"
            />
            <IconButton name="clipboard-outline" label="Colar link" onPress={() => void paste()} />
          </View>
          {error && <Text style={[type.caption, { color: colors.danger, marginTop: space.sm }]}>{error}</Text>}
          <View style={{ marginTop: space.md }}>{busy ? <ActivityIndicator color={colors.accent} /> : <Button title="Importar" onPress={() => void importLink()} disabled={!link} />}</View>
          <Text style={[type.micro, { marginTop: space.md, lineHeight: 16 }]}>
            Mudanças no Spotify (músicas novas ou removidas) chegam sozinhas em poucos minutos. O Spotify só libera playlists que você criou ou em que colabora.
          </Text>

          <SectionHeader title="Sincronizadas" />
          {list?.length === 0 && <Text style={type.caption}>Nenhuma playlist importada ainda.</Text>}
          {list?.map((p) => (
            <Pressable key={p.spotifyId} style={styles.row} onPress={() => router.push(`/playlist/${p.localId}`)}>
              <Artwork uri={p.artworkUrl} size={56} />
              <View style={{ flex: 1 }}>
                <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
                  {p.name}
                </Text>
                <Text style={type.caption}>
                  {p.playable} de {p.total} tocáveis · sincronizada {ago(p.lastSyncedAt)}
                </Text>
                {p.lastError && <Text style={[type.micro, { color: colors.danger }]}>{p.lastError}</Text>}
              </View>
              <IconButton name="sync-outline" label={`Sincronizar ${p.name}`} onPress={() => void syncOne(p)} />
              <IconButton name="close-circle-outline" color={colors.textDim} label={`Parar de sincronizar ${p.name}`} onPress={() => stop(p)} />
            </Pressable>
          ))}

          <View style={styles.note}>
            <Ionicons name="information-circle-outline" size={18} color={colors.textDim} />
            <Text style={[type.caption, { flex: 1 }]}>
              O áudio não vem do Spotify. Cada música toca a partir das suas fontes legais — seu acervo (Navidrome/Subsonic, em lossless) ou o catálogo Creative Commons. As que não forem encontradas aparecem na playlist com a opção de abrir no Spotify, e são procuradas de novo todo dia.
            </Text>
          </View>
          <Pressable onPress={() => void api.spotifyDisconnect().then(reloadStatus)} style={{ marginTop: space.lg }}>
            <Text style={[type.caption, { color: colors.danger, textAlign: 'center' }]}>Desconectar conta do Spotify</Text>
          </Pressable>
        </>
      )}
    </Screen>
  );
}

const styles = StyleSheet.create({
  inputRow: { flexDirection: 'row', alignItems: 'center', backgroundColor: colors.surface, borderRadius: radius.md, paddingLeft: space.md },
  input: { flex: 1, color: colors.text, fontSize: 15, minHeight: 48 },
  row: { flexDirection: 'row', alignItems: 'center', gap: space.md, paddingVertical: space.sm },
  note: { flexDirection: 'row', gap: space.sm, backgroundColor: colors.surface, borderRadius: radius.md, padding: space.md, marginTop: space.xl },
});
