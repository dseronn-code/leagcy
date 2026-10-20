import { useEffect, useMemo, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, TextInput, View } from 'react-native';
import { router } from 'expo-router';
import { Ionicons } from '@expo/vector-icons';
import { api } from '@/api/client';
import { useAsync, useDebounced } from '@/components/hooks';
import { TrackRow } from '@/components/TrackRow';
import { Artwork, Chip, Empty, Loading, Screen, SectionHeader } from '@/components/ui';
import { kvGet, kvSet } from '@/db/local';
import { playTracks } from '@/player/engine';
import { colors, radius, space, type } from '@/theme';

const GENRES = ['rock', 'pop', 'mpb', 'jazz', 'classical', 'electronic', 'hiphop', 'samba', 'blues', 'metal', 'folk', 'ambient', 'reggae', 'soul', 'funk', 'lofi'];
const GENRE_COLORS = ['#7C5CFF', '#E8505B', '#14B8A6', '#F59E0B', '#3B82F6', '#EC4899', '#22C55E', '#A855F7'];

type Tab = 'all' | 'track' | 'album' | 'artist';

export default function Search() {
  const [text, setText] = useState('');
  const [tab, setTab] = useState<Tab>('all');
  const [showFilters, setShowFilters] = useState(false);
  const [filters, setFilters] = useState({ genre: '', yearFrom: '', yearTo: '', quality: '' as '' | 'lossless' | 'hires' | 'spatial', lyrics: false });
  const [recent, setRecent] = useState<string[]>([]);

  useEffect(() => {
    void kvGet<string[]>('recentSearches').then((r) => r && setRecent(r));
  }, []);

  // Monta a query com a sintaxe avançada que o servidor entende (ver core/search.ts).
  const query = useMemo(() => {
    const parts: string[] = [];
    if (text.trim()) parts.push(filters.lyrics ? `lyrics:"${text.trim()}"` : text.trim());
    if (filters.genre) parts.push(`genre:${filters.genre}`);
    if (filters.yearFrom) parts.push(`year:${filters.yearFrom}${filters.yearTo ? `-${filters.yearTo}` : ''}`);
    if (filters.quality) parts.push(`quality:${filters.quality}`);
    if (tab !== 'all') parts.push(`type:${tab}`);
    return parts.join(' ');
  }, [text, filters, tab]);
  const q = useDebounced(query, 300);

  const { data, loading } = useAsync(async () => (q.replace(/type:\w+/, '').trim() ? api.search(q) : null), [q]);

  const remember = (s: string) => {
    const next = [s, ...recent.filter((r) => r !== s)].slice(0, 10);
    setRecent(next);
    void kvSet('recentSearches', next);
  };

  return (
    <Screen>
      <Text style={type.hero}>Buscar</Text>
      <View style={styles.inputWrap}>
        <Ionicons name="search" size={20} color={colors.textDim} />
        <TextInput
          style={styles.input}
          placeholder={filters.lyrics ? 'Trecho da letra…' : 'Artistas, músicas, álbuns… (ex.: artist:"Elis" year:1970-1979)'}
          placeholderTextColor={colors.textMuted}
          value={text}
          onChangeText={setText}
          onSubmitEditing={() => text && remember(text)}
          returnKeyType="search"
          autoCorrect={false}
          accessibilityLabel="Campo de busca"
        />
        <Pressable onPress={() => setShowFilters((s) => !s)} hitSlop={8} accessibilityLabel="Filtros avançados">
          <Ionicons name="options-outline" size={22} color={showFilters ? colors.accent : colors.textDim} />
        </Pressable>
      </View>

      {showFilters && (
        <View style={styles.filters}>
          <Text style={type.micro}>GÊNERO</Text>
          <ScrollView horizontal showsHorizontalScrollIndicator={false}>
            {GENRES.map((g) => (
              <Chip key={g} label={g} active={filters.genre === g} onPress={() => setFilters((f) => ({ ...f, genre: f.genre === g ? '' : g }))} />
            ))}
          </ScrollView>
          <Text style={type.micro}>ANO</Text>
          <View style={{ flexDirection: 'row', gap: space.sm }}>
            <TextInput style={styles.small} placeholder="De" placeholderTextColor={colors.textMuted} keyboardType="number-pad" maxLength={4} value={filters.yearFrom} onChangeText={(v) => setFilters((f) => ({ ...f, yearFrom: v }))} />
            <TextInput style={styles.small} placeholder="Até" placeholderTextColor={colors.textMuted} keyboardType="number-pad" maxLength={4} value={filters.yearTo} onChangeText={(v) => setFilters((f) => ({ ...f, yearTo: v }))} />
          </View>
          <Text style={type.micro}>QUALIDADE E LETRA</Text>
          <View style={{ flexDirection: 'row', flexWrap: 'wrap' }}>
            {(['lossless', 'hires', 'spatial'] as const).map((qv) => (
              <Chip key={qv} label={{ lossless: 'Lossless', hires: 'Hi-Res', spatial: 'Dolby Atmos' }[qv]} active={filters.quality === qv} onPress={() => setFilters((f) => ({ ...f, quality: f.quality === qv ? '' : qv }))} />
            ))}
            <Chip label="Buscar na letra" active={filters.lyrics} onPress={() => setFilters((f) => ({ ...f, lyrics: !f.lyrics }))} />
          </View>
        </View>
      )}

      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ marginVertical: space.md }}>
        {(['all', 'track', 'album', 'artist'] as const).map((t) => (
          <Chip key={t} label={{ all: 'Tudo', track: 'Músicas', album: 'Álbuns', artist: 'Artistas' }[t]} active={tab === t} onPress={() => setTab(t)} />
        ))}
      </ScrollView>

      {!q && recent.length > 0 && (
        <>
          <SectionHeader title="Buscas recentes" action="Limpar" onAction={() => (setRecent([]), void kvSet('recentSearches', []))} />
          {recent.map((r) => (
            <Pressable key={r} onPress={() => setText(r)} style={{ paddingVertical: space.sm }}>
              <Text style={type.body}>{r}</Text>
            </Pressable>
          ))}
        </>
      )}

      {!q && (
        <>
          <SectionHeader title="Navegar por gênero" />
          <View style={styles.grid}>
            {GENRES.map((g, i) => (
              <Pressable key={g} style={[styles.genre, { backgroundColor: GENRE_COLORS[i % GENRE_COLORS.length] }]} onPress={() => setFilters((f) => ({ ...f, genre: g }))} accessibilityRole="button">
                <Text style={[type.subtitle, { textTransform: 'capitalize' }]}>{g}</Text>
              </Pressable>
            ))}
          </View>
        </>
      )}

      {loading && q ? <Loading /> : null}

      {data && (
        <>
          {data.artists.length > 0 && (
            <>
              <SectionHeader title="Artistas" />
              <ScrollView horizontal showsHorizontalScrollIndicator={false}>
                {data.artists.map((a) => (
                  <Pressable key={a.id} style={{ width: 100, marginRight: space.md, alignItems: 'center' }} onPress={() => (remember(text), router.push(`/artist/${encodeURIComponent(a.id)}`))}>
                    <Artwork uri={a.imageUrl} size={88} round />
                    <Text style={[type.caption, { marginTop: space.xs, color: colors.text }]} numberOfLines={1}>
                      {a.name}
                    </Text>
                  </Pressable>
                ))}
              </ScrollView>
            </>
          )}
          {data.albums.length > 0 && (
            <>
              <SectionHeader title="Álbuns" />
              {data.albums.slice(0, tab === 'album' ? 50 : 4).map((a) => (
                <Pressable key={a.id} style={styles.albumRow} onPress={() => (remember(text), router.push(`/album/${encodeURIComponent(a.id)}`))}>
                  <Artwork uri={a.artworkUrl} size={56} />
                  <View style={{ flex: 1 }}>
                    <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
                      {a.title}
                    </Text>
                    <Text style={type.caption}>
                      {a.type === 'single' ? 'Single' : a.type === 'ep' ? 'EP' : 'Álbum'} · {a.artists[0]?.name} {a.year ? `· ${a.year}` : ''}
                    </Text>
                  </View>
                </Pressable>
              ))}
            </>
          )}
          {data.tracks.length > 0 && (
            <>
              <SectionHeader title="Músicas" />
              {data.tracks.map((t, i) => (
                <TrackRow key={t.id} track={t} onPress={() => (remember(text), void playTracks(data.tracks, i, { type: 'search' }, `Busca: ${text}`))} />
              ))}
            </>
          )}
          {!data.tracks.length && !data.albums.length && !data.artists.length && <Empty icon="search-outline" title="Nada encontrado" body="Tente outros termos ou remova filtros." />}
        </>
      )}
    </Screen>
  );
}

const styles = StyleSheet.create({
  inputWrap: { flexDirection: 'row', alignItems: 'center', gap: space.sm, backgroundColor: colors.surface, borderRadius: radius.md, paddingHorizontal: space.md, marginTop: space.lg, minHeight: 48 },
  input: { flex: 1, color: colors.text, fontSize: 16, paddingVertical: space.sm },
  filters: { backgroundColor: colors.surface, borderRadius: radius.md, padding: space.md, marginTop: space.sm, gap: space.sm },
  small: { flex: 1, backgroundColor: colors.surfaceHigh, color: colors.text, borderRadius: radius.sm, paddingHorizontal: space.md, minHeight: 40 },
  grid: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },
  genre: { width: '48.5%', height: 88, borderRadius: radius.md, padding: space.md, justifyContent: 'flex-end' },
  albumRow: { flexDirection: 'row', gap: space.md, alignItems: 'center', paddingVertical: space.sm },
});
