import { Pressable, StyleSheet, Switch, Text, View } from 'react-native';
import { router } from 'expo-router';
import type { AudioPreferences, StreamQuality } from '@sonora/core';
import { setToken } from '@/api/client';
import { useAsync } from '@/components/hooks';
import { IconButton, Screen, SectionHeader } from '@/components/ui';
import { outputInfo, setBitPerfect } from '@/player/dsp';
import { refreshSources } from '@/player/engine';
import { setAudioPrefs, useLibrary } from '@/library/store';
import { colors, radius, space, type } from '@/theme';

const QUALITIES: { id: StreamQuality; label: string; detail: string }[] = [
  { id: 'data-saver', label: 'Economia', detail: 'Opus/AAC ~96–128 kbps' },
  { id: 'high', label: 'Alta', detail: 'Opus/AAC até 320 kbps' },
  { id: 'lossless', label: 'Lossless', detail: 'FLAC/ALAC 16-bit/44,1 kHz (qualidade de CD)' },
  { id: 'hires', label: 'Hi-Res', detail: 'FLAC até 24-bit/192 kHz' },
  { id: 'max', label: 'Máxima', detail: 'Arquivo original, até 32-bit/384 kHz, sem transcodificação' },
];

function QualityPicker({ label, value, onChange }: { label: string; value: StreamQuality; onChange: (q: StreamQuality) => void }) {
  return (
    <View style={{ marginBottom: space.lg }}>
      <Text style={[type.micro, { marginBottom: space.sm }]}>{label.toUpperCase()}</Text>
      {QUALITIES.map((q) => (
        <Pressable key={q.id} onPress={() => onChange(q.id)} style={[styles.option, value === q.id && styles.optionActive]} accessibilityRole="radio" accessibilityState={{ checked: value === q.id }}>
          <View style={{ flex: 1 }}>
            <Text style={[type.body, { fontWeight: '600' }]}>{q.label}</Text>
            <Text style={type.caption}>{q.detail}</Text>
          </View>
          {value === q.id && <Text style={{ color: colors.accent, fontSize: 18 }}>●</Text>}
        </Pressable>
      ))}
    </View>
  );
}

function Toggle({ label, detail, value, onChange }: { label: string; detail?: string; value: boolean; onChange: (v: boolean) => void }) {
  return (
    <View style={styles.toggle}>
      <View style={{ flex: 1, paddingRight: space.md }}>
        <Text style={type.body}>{label}</Text>
        {detail && <Text style={type.caption}>{detail}</Text>}
      </View>
      <Switch value={value} onValueChange={onChange} trackColor={{ true: colors.accent, false: colors.surfaceHigh }} accessibilityLabel={label} />
    </View>
  );
}

export default function Settings() {
  const a = useLibrary((s) => s.audio);
  const { data: out } = useAsync(() => outputInfo(), []);
  const set = (p: Partial<AudioPreferences>) => {
    void setAudioPrefs(p);
    if (p.wifiQuality || p.cellularQuality || p.preferSpatial !== undefined) void refreshSources();
  };

  return (
    <Screen>
      <View style={{ flexDirection: 'row', alignItems: 'center' }}>
        <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
        <Text style={type.title}>Configurações</Text>
      </View>

      <SectionHeader title="Qualidade de áudio" />
      <QualityPicker label="Wi-Fi" value={a.wifiQuality} onChange={(q) => set({ wifiQuality: q })} />
      <QualityPicker label="Rede celular" value={a.cellularQuality} onChange={(q) => set({ cellularQuality: q })} />
      <QualityPicker label="Downloads" value={a.downloadQuality} onChange={(q) => set({ downloadQuality: q })} />

      <SectionHeader title="Reprodução" />
      <Toggle label="Gapless" detail="Sem silêncio entre faixas (álbuns ao vivo e conceituais)" value={a.gapless} onChange={(v) => set({ gapless: v })} />
      <Toggle label="Preferir Dolby Atmos" detail="Usa a mixagem espacial quando disponível" value={a.preferSpatial} onChange={(v) => set({ preferSpatial: v })} />
      <View style={styles.toggle}>
        <View style={{ flex: 1 }}>
          <Text style={type.body}>Normalização de volume</Text>
          <Text style={type.caption}>ReplayGain sem compressão: só atenua, nunca clipa</Text>
        </View>
      </View>
      <View style={{ flexDirection: 'row', flexWrap: 'wrap', gap: space.sm, marginBottom: space.md }}>
        {(['off', 'auto', 'track', 'album'] as const).map((m) => (
          <Pressable key={m} onPress={() => set({ normalization: m })} style={[styles.pill, a.normalization === m && styles.optionActive]}>
            <Text style={type.caption}>{{ off: 'Desligada', auto: 'Automática', track: 'Por faixa', album: 'Por álbum' }[m]}</Text>
          </Pressable>
        ))}
      </View>
      <View style={{ flexDirection: 'row', gap: space.sm, marginBottom: space.md }}>
        {[-23, -16, -14, -11].map((l) => (
          <Pressable key={l} onPress={() => set({ normalizationTargetLufs: l })} style={[styles.pill, a.normalizationTargetLufs === l && styles.optionActive]}>
            <Text style={type.caption}>{l} LUFS</Text>
          </Pressable>
        ))}
      </View>
      <Toggle
        label="Saída bit-perfect (DAC USB)"
        detail="Android 14+: envia o áudio ao DAC sem passar pelo mixer do sistema"
        value={a.bitPerfectUsb}
        onChange={(v) => {
          set({ bitPerfectUsb: v });
          void setBitPerfect(v);
        }}
      />
      <Pressable onPress={() => router.push('/equalizer')} style={styles.toggle}>
        <Text style={type.body}>Equalizador</Text>
      </Pressable>
      <View style={[styles.option, { marginTop: space.md }]}>
        <Text style={type.caption}>{out ? `Saída: ${out.device} · ${out.bitDepth}-bit/${out.sampleRateHz / 1000} kHz${out.bitPerfect ? ' · bit-perfect' : ''}` : 'Saída: mixer do sistema (instale o build com o módulo SonoraDsp para detalhes)'}</Text>
      </View>

      <SectionHeader title="Armazenamento" />
      <Pressable onPress={() => router.push('/downloads')} style={styles.toggle}>
        <Text style={type.body}>Downloads e espaço usado</Text>
      </Pressable>

      <SectionHeader title="Conta" />
      <Pressable
        onPress={() => {
          void setToken(null).then(() => router.replace('/login'));
        }}
        style={styles.toggle}
      >
        <Text style={[type.body, { color: colors.danger }]}>Sair</Text>
      </Pressable>
      <Text style={[type.micro, { marginTop: space.xl, textAlign: 'center' }]}>Sonora 0.1 · sem anúncios, sem rastreamento publicitário</Text>
    </Screen>
  );
}

const styles = StyleSheet.create({
  option: { flexDirection: 'row', alignItems: 'center', padding: space.md, borderRadius: radius.md, backgroundColor: colors.surface, marginBottom: space.xs, minHeight: 56 },
  optionActive: { backgroundColor: colors.accentSoft, borderColor: colors.accent, borderWidth: 1 },
  toggle: { flexDirection: 'row', alignItems: 'center', minHeight: 56, borderBottomWidth: StyleSheet.hairlineWidth, borderColor: colors.border },
  pill: { paddingHorizontal: space.md, paddingVertical: space.sm, borderRadius: radius.pill, backgroundColor: colors.surface, borderWidth: 1, borderColor: 'transparent' },
});
