import { useCallback, useMemo, useState } from 'react';
import { Pressable, ScrollView, StyleSheet, Text, useWindowDimensions, View } from 'react-native';
import { router, useFocusEffect } from 'expo-router';
import { LinearGradient } from 'expo-linear-gradient';
import { listenedMsByPeriod, listeningClock, localDay, summarize, type PlayEvent, type Track } from '@sonora/core';
import { BarChart, Heatmap } from '@/components/charts';
import { Artwork, Chip, Empty, formatMinutes, Screen, SectionHeader } from '@/components/ui';
import { cachedTracks, playsBetween } from '@/db/local';
import { colors, radius, space, type } from '@/theme';

type Range = 'today' | 'week' | 'month' | 'year' | 'all';
const DAY = 86_400_000;

function rangeBounds(r: Range, now: number): [number, number] {
  const d = new Date(now);
  const startOfDay = new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime();
  switch (r) {
    case 'today':
      return [startOfDay, now + 1];
    case 'week':
      return [startOfDay - 6 * DAY, now + 1];
    case 'month':
      return [startOfDay - 29 * DAY, now + 1];
    case 'year':
      return [new Date(d.getFullYear(), 0, 1).getTime(), now + 1];
    case 'all':
      return [0, now + 1];
  }
}

/**
 * Estatísticas calculadas no aparelho a partir do SQLite local: instantâneas,
 * funcionam offline e usam exatamente o mesmo código do servidor (@sonora/core).
 */
export default function Stats() {
  const { width } = useWindowDimensions();
  const [range, setRange] = useState<Range>('month');
  const [history, setHistory] = useState<PlayEvent[]>([]);
  const [tracks, setTracks] = useState<Map<string, Track>>(new Map());

  useFocusEffect(
    useCallback(() => {
      void playsBetween(0, Date.now() + 1).then(async (h) => {
        setHistory(h);
        setTracks(await cachedTracks([...new Set(h.map((e) => e.trackId))].slice(-2000)));
      });
    }, []),
  );

  const now = Date.now();
  const tz = -new Date().getTimezoneOffset();
  const [from, to] = rangeBounds(range, now);
  const s = useMemo(() => summarize(history, from, to, localDay(now, tz)), [history, from, to]); // eslint-disable-line react-hooks/exhaustive-deps
  const inRange = useMemo(() => history.filter((e) => e.startedAt >= from && e.startedAt < to), [history, from, to]);

  const series = useMemo(() => {
    const period = range === 'year' || range === 'all' ? 'month' : 'day';
    const m = listenedMsByPeriod(inRange, period);
    if (range === 'today') {
      const hours = new Array<number>(24).fill(0);
      for (const e of inRange) hours[new Date(e.startedAt).getHours()]! += e.listenedMs;
      return hours.map((ms, h) => ({ label: `${h}h`, value: Math.round(ms / 60_000) }));
    }
    if (period === 'day') {
      const days = range === 'week' ? 7 : 30;
      return Array.from({ length: days }, (_, i) => {
        const k = localDay(now - (days - 1 - i) * DAY, tz);
        return { label: k.slice(8), value: Math.round((m.get(k) ?? 0) / 60_000) };
      });
    }
    return [...m.entries()].sort(([a], [b]) => (a < b ? -1 : 1)).map(([k, ms]) => ({ label: k.slice(2), value: Math.round(ms / 60_000) }));
  }, [inRange, range]); // eslint-disable-line react-hooks/exhaustive-deps

  const artistName = (id: string) => {
    for (const t of tracks.values()) {
      const a = t.artists.find((x) => x.id === id);
      if (a) return a.name;
    }
    return id;
  };
  const artistArt = (id: string) => [...tracks.values()].find((t) => t.artists.some((a) => a.id === id))?.artworkUrl;

  if (!history.length) {
    return (
      <Screen>
        <Text style={type.hero}>Estatísticas</Text>
        <Empty icon="stats-chart-outline" title="Comece a ouvir" body="Cada minuto ouvido aparece aqui — por dia, semana, mês e ano." />
      </Screen>
    );
  }

  return (
    <Screen>
      <Text style={type.hero}>Estatísticas</Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ marginVertical: space.md }}>
        {(['today', 'week', 'month', 'year', 'all'] as const).map((r) => (
          <Chip key={r} label={{ today: 'Hoje', week: '7 dias', month: '30 dias', year: 'Este ano', all: 'Sempre' }[r]} active={range === r} onPress={() => setRange(r)} />
        ))}
      </ScrollView>

      <LinearGradient colors={['#4C2FD9', '#1B1240']} style={styles.hero}>
        <Text style={type.micro}>TEMPO OUVIDO</Text>
        <Text style={[type.hero, { fontSize: 44 }]}>{s.totalMinutes.toLocaleString('pt-BR')} min</Text>
        <Text style={type.caption}>
          {s.totalHours.toLocaleString('pt-BR')} horas · {s.qualifiedPlays} reproduções · {s.uniqueTracks} músicas · {s.uniqueArtists} artistas
        </Text>
        <View style={styles.kpis}>
          <Kpi label="Sequência atual" value={`${s.streak.current} dias`} />
          <Kpi label="Maior sequência" value={`${s.streak.longest} dias`} />
          <Kpi label="Taxa de pulos" value={`${Math.round(s.skipRate * 100)}%`} />
        </View>
      </LinearGradient>

      <SectionHeader title="Minutos por período" />
      <BarChart data={series} width={width - space.lg * 2} />

      <SectionHeader title="Artistas mais ouvidos" />
      {s.topArtists.slice(0, 5).map((a, i) => (
        <Pressable key={a.id} style={styles.rank} onPress={() => router.push(`/artist/${encodeURIComponent(a.id)}`)}>
          <Text style={[type.subtitle, styles.pos]}>{i + 1}</Text>
          <Artwork uri={artistArt(a.id)} size={48} round />
          <View style={{ flex: 1 }}>
            <Text style={[type.body, { fontWeight: '600' }]}>{artistName(a.id)}</Text>
            <Text style={type.caption}>
              {formatMinutes(a.listenedMs)} · {a.plays} plays
            </Text>
          </View>
        </Pressable>
      ))}

      <SectionHeader title="Músicas mais tocadas" />
      {s.topTracks.slice(0, 10).map((r, i) => {
        const t = tracks.get(r.id);
        return (
          <View key={r.id} style={styles.rank}>
            <Text style={[type.subtitle, styles.pos]}>{i + 1}</Text>
            <Artwork uri={t?.artworkUrl} size={48} />
            <View style={{ flex: 1 }}>
              <Text style={[type.body, { fontWeight: '600' }]} numberOfLines={1}>
                {t?.title ?? r.id}
              </Text>
              <Text style={type.caption}>
                {t?.artists[0]?.name} · {r.plays} plays · {formatMinutes(r.listenedMs)}
              </Text>
            </View>
          </View>
        );
      })}

      <SectionHeader title="Gêneros" />
      {s.topGenres.map((g) => {
        const pct = s.totalMs ? g.listenedMs / s.totalMs : 0;
        return (
          <View key={g.id} style={{ marginBottom: space.sm }}>
            <View style={{ flexDirection: 'row', justifyContent: 'space-between' }}>
              <Text style={[type.body, { textTransform: 'capitalize' }]}>{g.id}</Text>
              <Text style={type.caption}>{Math.round(pct * 100)}%</Text>
            </View>
            <View style={styles.barBg}>
              <View style={[styles.barFill, { width: `${pct * 100}%` }]} />
            </View>
          </View>
        );
      })}

      <SectionHeader title="Quando você ouve" />
      <Heatmap grid={listeningClock(inRange)} width={width - space.lg * 2} />

      <SectionHeader title="Destaques" />
      <View style={styles.cards}>
        {s.peakDay && <Card label="Dia com mais música" value={new Date(`${s.peakDay.day}T12:00:00`).toLocaleDateString('pt-BR', { day: '2-digit', month: 'short' })} sub={formatMinutes(s.peakDay.listenedMs)} />}
        {s.peakHour !== null && <Card label="Horário favorito" value={`${s.peakHour}h`} sub="pico de escuta" />}
        <Card label="Artistas novos" value={String(s.newArtists.length)} sub="descobertos no período" />
      </View>
      <Pressable onPress={() => router.push('/history')} style={{ marginTop: space.lg }}>
        <Text style={[type.body, { color: colors.accent, textAlign: 'center' }]}>Ver histórico completo</Text>
      </Pressable>
    </Screen>
  );
}

function Kpi({ label, value }: { label: string; value: string }) {
  return (
    <View style={{ flex: 1 }}>
      <Text style={[type.subtitle]}>{value}</Text>
      <Text style={type.micro}>{label}</Text>
    </View>
  );
}

function Card({ label, value, sub }: { label: string; value: string; sub: string }) {
  return (
    <View style={styles.card}>
      <Text style={type.micro}>{label.toUpperCase()}</Text>
      <Text style={type.title}>{value}</Text>
      <Text style={type.caption}>{sub}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  hero: { borderRadius: radius.lg, padding: space.xl, gap: space.xs },
  kpis: { flexDirection: 'row', marginTop: space.lg, gap: space.md },
  rank: { flexDirection: 'row', alignItems: 'center', gap: space.md, paddingVertical: space.sm },
  pos: { width: 24, textAlign: 'center', color: colors.textDim },
  barBg: { height: 6, backgroundColor: colors.surfaceHigh, borderRadius: 3, marginTop: 4 },
  barFill: { height: 6, backgroundColor: colors.accent, borderRadius: 3 },
  cards: { flexDirection: 'row', flexWrap: 'wrap', gap: space.sm },
  card: { width: '48.5%', backgroundColor: colors.surface, borderRadius: radius.md, padding: space.lg, gap: 2 },
});
