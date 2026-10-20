import { useState } from 'react';
import { ScrollView, StyleSheet, Switch, Text, View } from 'react-native';
import { router } from 'expo-router';
import { Chip, IconButton, Screen } from '@/components/ui';
import { dspAvailable, EQ_BANDS_HZ, EQ_PRESETS, setEq, useEq } from '@/player/dsp';
import { colors, radius, space, type } from '@/theme';

const MAX_DB = 12;
const H = 200;

function Band({ hz, value, onChange, disabled }: { hz: number; value: number; onChange: (v: number) => void; disabled: boolean }) {
  const [height, setHeight] = useState(H);
  const toDb = (y: number) => Math.round(Math.max(-MAX_DB, Math.min(MAX_DB, MAX_DB - (y / height) * 2 * MAX_DB)) * 2) / 2;
  const pct = (MAX_DB - value) / (2 * MAX_DB);
  const label = hz >= 1000 ? `${hz / 1000}k` : String(hz);
  return (
    <View style={{ alignItems: 'center', flex: 1 }}>
      <Text style={[type.micro, { marginBottom: space.xs }]}>{value > 0 ? `+${value}` : value}</Text>
      <View
        style={styles.slider}
        onLayout={(e) => setHeight(e.nativeEvent.layout.height)}
        onStartShouldSetResponder={() => !disabled}
        onResponderGrant={(e) => onChange(toDb(e.nativeEvent.locationY))}
        onResponderMove={(e) => onChange(toDb(e.nativeEvent.locationY))}
        accessible
        accessibilityRole="adjustable"
        accessibilityLabel={`${label} hertz`}
        accessibilityValue={{ min: -MAX_DB, max: MAX_DB, now: value, text: `${value} dB` }}
        accessibilityActions={[{ name: 'increment' }, { name: 'decrement' }]}
        onAccessibilityAction={(e) => onChange(Math.max(-MAX_DB, Math.min(MAX_DB, value + (e.nativeEvent.actionName === 'increment' ? 1 : -1))))}
      >
        <View style={styles.zero} />
        <View style={[styles.knob, { top: pct * (height - 18), opacity: disabled ? 0.4 : 1 }]} />
      </View>
      <Text style={[type.micro, { marginTop: space.xs }]}>{label}</Text>
    </View>
  );
}

export default function Equalizer() {
  const eq = useEq();
  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <Text style={[type.title, { flex: 1 }]}>Equalizador</Text>
        <Switch value={eq.enabled} onValueChange={(v) => setEq({ enabled: v })} trackColor={{ true: colors.accent, false: colors.surfaceHigh }} accessibilityLabel="Ativar equalizador" />
      </View>
      {!dspAvailable && <Text style={[type.caption, styles.note]}>O processamento roda no módulo nativo SonoraDsp (build de desenvolvimento/produção). Neste build as configurações são salvas, mas o áudio sai sem EQ.</Text>}
      <Text style={[type.caption, { marginVertical: space.md }]}>
        Desligado = caminho bit-perfect. Ligado, um preamp de {eq.preampDb} dB evita clipping.
      </Text>
      <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ marginBottom: space.lg }}>
        {Object.keys(EQ_PRESETS).map((p) => (
          <Chip key={p} label={p} active={eq.preset === p} onPress={() => setEq({ preset: p, gains: EQ_PRESETS[p]!, enabled: true })} />
        ))}
      </ScrollView>
      <View style={styles.bands}>
        {EQ_BANDS_HZ.map((hz, i) => (
          <Band key={hz} hz={hz} value={eq.gains[i] ?? 0} disabled={!eq.enabled} onChange={(v) => setEq({ gains: eq.gains.map((g, j) => (j === i ? v : g)), preset: 'Personalizado' })} />
        ))}
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  note: { backgroundColor: colors.surface, padding: space.md, borderRadius: radius.md, marginTop: space.md },
  bands: { flexDirection: 'row', backgroundColor: colors.surface, borderRadius: radius.lg, paddingVertical: space.lg, paddingHorizontal: space.xs },
  slider: { width: 28, height: H, alignItems: 'center' },
  zero: { position: 'absolute', top: H / 2, width: 28, height: 1, backgroundColor: colors.border },
  knob: { position: 'absolute', width: 18, height: 18, borderRadius: 9, backgroundColor: colors.accent },
});
