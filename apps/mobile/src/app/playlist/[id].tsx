import { useMemo, useState } from 'react';
import { Alert, Linking, ScrollView, Share, Text, TextInput, View } from 'react-native';
import { router, useLocalSearchParams } from 'expo-router';
import { evaluateSmartPlaylist, isQualifiedPlay, type Track } from '@sonora/core';
import { api } from '@/api/client';
import { CollectionHeader, PlayButtons } from '@/components/CollectionHeader';
import { useAsync } from '@/components/hooks';
import { ReorderableList } from '@/components/ReorderableList';
import { TrackRow } from '@/components/TrackRow';
import { Empty, formatMinutes, IconButton, SectionHeader } from '@/components/ui';
import { playsBetween } from '@/db/local';
import { download } from '@/downloads/manager';
import { addToPlaylist, deletePlaylist, makeCollaborative, moveEntry, removeFromPlaylist, sortPlaylist, updatePlaylist, useLibrary } from '@/library/store';
import { useTracks } from '@/library/useTracks';
import { playTracks } from '@/player/engine';
import { colors, radius, space, type } from '@/theme';

const ROW = 64;

export default function PlaylistScreen() {
  const { id } = useLocalSearchParams<{ id: string }>();
  const playlist = useLibrary((s) => s.playlists.find((p) => p.id === id));
  const likes = useLibrary((s) => s.likes);
  const [editing, setEditing] = useState(false);
  const [scrollEnabled, setScrollEnabled] = useState(true);

  const isLiked = id === 'liked';
  const isSmart = playlist?.kind === 'smart';

  // IDs a resolver: curtidas, entradas da playlist, ou (smart) curtidas + histórico.
  const { data: history } = useAsync(() => (isSmart ? playsBetween(0, Date.now() + 1) : Promise.resolve([])), [isSmart]);
  const ids = useMemo(() => {
    if (isLiked) return [...likes.entries()].sort((a, b) => b[1] - a[1]).map(([tid]) => tid);
    if (isSmart) return [...new Set([...likes.keys(), ...(history ?? []).map((e) => e.trackId)])];
    return playlist?.entries.map((e) => e.trackId) ?? [];
  }, [isLiked, isSmart, likes, history, playlist]);
  const { tracks: resolved } = useTracks(ids);

  // Linhas exibidas: em playlists normais, uma por entrada (a mesma faixa pode aparecer 2x).
  const rows = useMemo(() => {
    if (isLiked || isSmart || !playlist) return null;
    const byId = new Map(resolved.map((t) => [t.id, t]));
    return playlist.entries.map((e) => ({ e, t: byId.get(e.trackId) })).filter((x): x is { e: (typeof playlist.entries)[number]; t: Track } => !!x.t);
  }, [isLiked, isSmart, playlist, resolved]);

  const smartRules = playlist?.smartRules;
  const tracks: Track[] = useMemo(() => {
    if (rows) return rows.map((r) => r.t);
    if (!isSmart || !smartRules) return resolved;
    const stats = new Map<string, { playCount: number; lastPlayedAt?: number }>();
    for (const e of history ?? []) {
      const s = stats.get(e.trackId) ?? { playCount: 0 };
      if (isQualifiedPlay(e)) s.playCount++;
      s.lastPlayedAt = Math.max(s.lastPlayedAt ?? 0, e.startedAt);
      stats.set(e.trackId, s);
    }
    return evaluateSmartPlaylist(resolved, (tid) => ({ playCount: stats.get(tid)?.playCount ?? 0, lastPlayedAt: stats.get(tid)?.lastPlayedAt, liked: likes.has(tid) }), smartRules);
  }, [rows, isSmart, resolved, smartRules, history, likes]);

  // Faixas do Spotify sem fonte legal ficam visíveis, mas fora da fila.
  const playable = useMemo(() => tracks.filter((t) => t.provider !== 'spotify'), [tracks]);
  const canEdit = !!playlist && playlist.kind !== 'smart' && playlist.kind !== 'algorithmic' && playlist.kind !== 'spotify';
  const fromSpotify = playlist?.kind === 'spotify';
  const { data: recs } = useAsync(async () => (canEdit && tracks.length ? (await api.playlistRecs(tracks.map((t) => t.id))).tracks : []), [canEdit, tracks.length]);

  if (!isLiked && !playlist) return <Empty icon="musical-notes-outline" title="Playlist não encontrada" />;
  const title = isLiked ? 'Músicas curtidas' : playlist!.name;
  const ctx = { type: 'playlist' as const, id };
  const total = tracks.reduce((s, t) => s + t.durationMs, 0);

  const sortBy = (key: 'title' | 'artist' | 'added') => {
    if (!playlist) return;
    const byTrack = new Map(tracks.map((t) => [t.id, t]));
    const entries = [...playlist.entries];
    entries.sort((a, b) => {
      if (key === 'added') return b.addedAt - a.addedAt;
      const ta = byTrack.get(a.trackId);
      const tb = byTrack.get(b.trackId);
      const va = key === 'title' ? ta?.title : ta?.artists[0]?.name;
      const vb = key === 'title' ? tb?.title : tb?.artists[0]?.name;
      return (va ?? '').localeCompare(vb ?? '', 'pt-BR');
    });
    void sortPlaylist(playlist.id, entries.map((e) => e.entryId));
  };

  const more = () =>
    Alert.alert(title, undefined, [
      { text: 'Ordenar por título', onPress: () => sortBy('title') },
      { text: 'Ordenar por artista', onPress: () => sortBy('artist') },
      { text: 'Ordenar por data de adição', onPress: () => sortBy('added') },
      ...(playlist?.kind === 'user' ? [{ text: 'Tornar colaborativa', onPress: () => void makeCollaborative(playlist.id) }] : []),
      { text: playlist?.isPublic ? 'Tornar privada' : 'Tornar pública', onPress: () => playlist && void updatePlaylist(playlist.id, { isPublic: !playlist.isPublic }) },
      { text: 'Excluir playlist', style: 'destructive', onPress: () => playlist && void deletePlaylist(playlist.id).then(() => router.back()) },
      { text: 'Cancelar', style: 'cancel' },
    ]);

  const share = async () => {
    if (playlist?.kind === 'collaborative') {
      const { url } = await api.invite(playlist.id);
      await Share.share({ message: `Edite a playlist "${playlist.name}" comigo no Sonora: ${url}` });
    } else await Share.share({ message: `${title} no Sonora: sonora://playlist/${id}` });
  };

  return (
    <ScrollView style={{ backgroundColor: colors.bg }} scrollEnabled={scrollEnabled} contentContainerStyle={{ paddingBottom: 120 }}>
      <CollectionHeader
        artwork={playlist?.artworkUrl ?? tracks[0]?.artworkUrl}
        title={title}
        subtitle={playlist?.description}
        meta={`${playlist?.kind === 'collaborative' ? 'Colaborativa · ' : isSmart ? 'Inteligente · ' : fromSpotify ? 'Sincronizada do Spotify · ' : ''}${fromSpotify ? `${playable.length} de ${tracks.length} tocáveis` : `${tracks.length} músicas`} · ${formatMinutes(total)}`}
        actions={
          <>
            {canEdit && <IconButton name={editing ? 'checkmark' : 'create-outline'} color={editing ? colors.accent : colors.text} label={editing ? 'Concluir edição' : 'Editar'} onPress={() => setEditing(!editing)} />}
            <IconButton name="arrow-down-circle-outline" label="Baixar todas" onPress={() => download(tracks)} />
            {!isLiked && !fromSpotify && <IconButton name={playlist?.kind === 'collaborative' ? 'person-add-outline' : 'share-outline'} label="Compartilhar" onPress={() => void share()} />}
            {!isLiked && !fromSpotify && <IconButton name="ellipsis-horizontal" label="Mais" onPress={more} />}
            {fromSpotify && <IconButton name="sync-circle-outline" label="Gerenciar sincronização com o Spotify" onPress={() => router.push('/spotify')} />}
            <PlayButtons onPlay={() => void playTracks(playable, 0, ctx, title, { shuffle: false })} onShuffle={() => void playTracks(playable, 0, ctx, title, { shuffle: true })} />
          </>
        }
      />
      <View style={{ paddingHorizontal: space.lg }}>
        {editing && playlist && (
          <TextInput
            defaultValue={playlist.name}
            onEndEditing={(e) => e.nativeEvent.text.trim() && void updatePlaylist(playlist.id, { name: e.nativeEvent.text.trim() })}
            style={{ backgroundColor: colors.surface, color: colors.text, borderRadius: radius.md, padding: space.md, fontSize: 18, marginBottom: space.md }}
            accessibilityLabel="Nome da playlist"
          />
        )}
        {fromSpotify && (
          <Text style={[type.caption, { marginBottom: space.sm }]}>Para adicionar ou tirar músicas, edite a playlist no Spotify — as mudanças chegam aqui sozinhas.</Text>
        )}
        {tracks.length === 0 && <Empty icon="add-circle-outline" title="Playlist vazia" body="Toque longo em qualquer música e escolha “Adicionar à playlist”." />}
        {editing && playlist && rows ? (
          <ReorderableList
            items={rows}
            keyOf={(x) => x.e.entryId}
            rowHeight={ROW}
            onDragStateChange={(d) => setScrollEnabled(!d)}
            onMove={(from, to) => {
              // Índices da lista visível → índices reais (entradas sem metadados ficam de fora da lista).
              const idx = (i: number) => playlist.entries.findIndex((e) => e.entryId === rows[i]!.e.entryId);
              void moveEntry(playlist.id, idx(from), idx(to));
            }}
            renderRow={({ e, t }, _i, handle) => (
              <View style={{ flexDirection: 'row', alignItems: 'center' }}>
                <IconButton name="remove-circle" color={colors.danger} label={`Remover ${t.title}`} onPress={() => void removeFromPlaylist(playlist.id, e.entryId)} />
                <View style={{ flex: 1 }}>
                  <TrackRow track={t} onPress={() => undefined} trailing={<View />} />
                </View>
                {handle}
              </View>
            )}
          />
        ) : (
          tracks.map((t, i) =>
            t.provider === 'spotify' ? (
              <View key={rows?.[i]?.e.entryId ?? t.id} style={{ opacity: 0.45 }}>
                <TrackRow
                  track={t}
                  onPress={() => void Linking.openURL(`https://open.spotify.com/track/${t.id.slice('spotify:'.length)}`)}
                  trailing={<IconButton name="open-outline" size={18} color={colors.textDim} label={`${t.title}: não está no seu acervo. Abrir no Spotify`} onPress={() => void Linking.openURL(`https://open.spotify.com/track/${t.id.slice('spotify:'.length)}`)} />}
                />
              </View>
            ) : (
              <TrackRow key={rows?.[i]?.e.entryId ?? t.id} track={t} onPress={() => void playTracks(playable, Math.max(0, playable.indexOf(t)), ctx, title)} />
            ),
          )
        )}

        {!editing && recs && recs.length > 0 && playlist && (
          <>
            <SectionHeader title="Recomendadas" />
            <Text style={[type.caption, { marginTop: -space.sm, marginBottom: space.sm }]}>Baseadas nas músicas desta playlist</Text>
            {recs.map((t) => (
              <TrackRow key={t.id} track={t} onPress={() => void playTracks(recs, recs.indexOf(t), ctx, title)} trailing={<IconButton name="add-circle-outline" label={`Adicionar ${t.title}`} onPress={() => void addToPlaylist(playlist.id, [t])} />} />
            ))}
          </>
        )}
      </View>
    </ScrollView>
  );
}
