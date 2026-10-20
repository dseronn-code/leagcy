import { type ReactNode } from 'react';
import { ActivityIndicator, Pressable, ScrollView, StyleSheet, Text, View, type PressableProps, type ViewStyle } from 'react-native';
import { Image } from 'expo-image';
import { Ionicons } from '@expo/vector-icons';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { isHiRes, isLossless, qualityLabel, type AudioSource } from '@sonora/core';
import { colors, HIT, radius, space, type } from '@/theme';

export type IconName = keyof typeof Ionicons.glyphMap;

export function Screen({ children, scroll = true, padded = true }: { children: ReactNode; scroll?: boolean; padded?: boolean }) {
  const insets = useSafeAreaInsets();
  const style: ViewStyle = { paddingTop: insets.top + space.sm, paddingHorizontal: padded ? space.lg : 0, paddingBottom: 140 };
  return scroll ? (
    <ScrollView style={styles.screen} contentContainerStyle={style} keyboardShouldPersistTaps="handled">
      {children}
    </ScrollView>
  ) : (
    <View style={[styles.screen, style]}>{children}</View>
  );
}

export function IconButton({ name, size = 24, color = colors.text, label, ...rest }: { name: IconName; size?: number; color?: string; label: string } & PressableProps) {
  return (
    <Pressable accessibilityRole="button" accessibilityLabel={label} hitSlop={8} style={({ pressed }) => [styles.iconBtn, pressed && { opacity: 0.5 }]} {...rest}>
      <Ionicons name={name} size={size} color={color} />
    </Pressable>
  );
}

export function Button({ title, onPress, icon, variant = 'primary', disabled }: { title: string; onPress: () => void; icon?: IconName; variant?: 'primary' | 'secondary'; disabled?: boolean }) {
  const primary = variant === 'primary';
  return (
    <Pressable
      accessibilityRole="button"
      disabled={disabled}
      onPress={onPress}
      style={({ pressed }) => [styles.button, primary ? { backgroundColor: colors.accent } : { borderColor: colors.border, borderWidth: 1 }, (pressed || disabled) && { opacity: 0.6 }]}
    >
      {icon && <Ionicons name={icon} size={18} color={colors.text} />}
      <Text style={[type.body, { fontWeight: '700' }]}>{title}</Text>
    </Pressable>
  );
}

export function Artwork({ uri, size, round }: { uri?: string; size: number; round?: boolean }) {
  return (
    <Image
      source={uri ? { uri } : undefined}
      style={{ width: size, height: size, borderRadius: round ? size / 2 : radius.sm, backgroundColor: colors.surfaceHigh }}
      contentFit="cover"
      transition={150}
      cachePolicy="memory-disk"
      recyclingKey={uri}
      accessibilityIgnoresInvertColors
    />
  );
}

export function QualityBadge({ source, compact }: { source?: AudioSource | null; compact?: boolean }) {
  if (!source) return null;
  const hires = isHiRes(source);
  const lossless = isLossless(source);
  const color = source.spatial ? colors.accent : hires ? colors.hires : lossless ? colors.lossless : colors.textMuted;
  const text = compact ? (source.spatial ? 'ATMOS' : hires ? 'HI-RES' : lossless ? 'LOSSLESS' : source.codec.toUpperCase()) : qualityLabel(source);
  return (
    <View style={[styles.badge, { borderColor: color }]} accessibilityLabel={`Qualidade: ${qualityLabel(source)}`}>
      <Text style={[type.micro, { color, fontWeight: '700' }]}>{text}</Text>
    </View>
  );
}

export function SectionHeader({ title, action, onAction }: { title: string; action?: string; onAction?: () => void }) {
  return (
    <View style={styles.sectionHeader}>
      <Text style={type.title} accessibilityRole="header">
        {title}
      </Text>
      {action && (
        <Pressable onPress={onAction} hitSlop={8}>
          <Text style={[type.caption, { color: colors.accent }]}>{action}</Text>
        </Pressable>
      )}
    </View>
  );
}

export function MediaCard({ title, subtitle, uri, onPress, round, size = 148 }: { title: string; subtitle?: string; uri?: string; onPress: () => void; round?: boolean; size?: number }) {
  return (
    <Pressable onPress={onPress} style={{ width: size, marginRight: space.md }} accessibilityRole="button" accessibilityLabel={`${title}${subtitle ? `, ${subtitle}` : ''}`}>
      <Artwork uri={uri} size={size} round={round} />
      <Text style={[type.body, { marginTop: space.sm, fontWeight: '600' }]} numberOfLines={1}>
        {title}
      </Text>
      {subtitle && (
        <Text style={type.caption} numberOfLines={2}>
          {subtitle}
        </Text>
      )}
    </Pressable>
  );
}

export function HRow({ children }: { children: ReactNode }) {
  return (
    <ScrollView horizontal showsHorizontalScrollIndicator={false} style={{ marginHorizontal: -space.lg }} contentContainerStyle={{ paddingHorizontal: space.lg }}>
      {children}
    </ScrollView>
  );
}

export function Loading() {
  return (
    <View style={{ padding: space.xxl, alignItems: 'center' }}>
      <ActivityIndicator color={colors.accent} />
    </View>
  );
}

export function Empty({ icon, title, body }: { icon: IconName; title: string; body?: string }) {
  return (
    <View style={{ alignItems: 'center', padding: space.xxl, gap: space.sm }}>
      <Ionicons name={icon} size={40} color={colors.textMuted} />
      <Text style={type.subtitle}>{title}</Text>
      {body && <Text style={[type.caption, { textAlign: 'center' }]}>{body}</Text>}
    </View>
  );
}

export function Chip({ label, active, onPress }: { label: string; active?: boolean; onPress: () => void }) {
  return (
    <Pressable
      onPress={onPress}
      accessibilityRole="button"
      accessibilityState={{ selected: !!active }}
      style={[styles.chip, active && { backgroundColor: colors.accent, borderColor: colors.accent }]}
    >
      <Text style={[type.caption, { color: colors.text, fontWeight: active ? '700' : '500' }]}>{label}</Text>
    </Pressable>
  );
}

export function formatDuration(ms: number): string {
  const s = Math.max(0, Math.round(ms / 1000));
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  return h ? `${h}:${String(m).padStart(2, '0')}:${String(sec).padStart(2, '0')}` : `${m}:${String(sec).padStart(2, '0')}`;
}

export function formatMinutes(ms: number): string {
  const min = Math.floor(ms / 60_000);
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60);
  return `${h.toLocaleString('pt-BR')} h ${min % 60} min`;
}

const styles = StyleSheet.create({
  screen: { flex: 1, backgroundColor: colors.bg },
  iconBtn: { minWidth: HIT, minHeight: HIT, alignItems: 'center', justifyContent: 'center' },
  button: { flexDirection: 'row', gap: space.sm, alignItems: 'center', justifyContent: 'center', paddingHorizontal: space.xl, minHeight: HIT, borderRadius: radius.pill },
  badge: { borderWidth: 1, borderRadius: 4, paddingHorizontal: 5, paddingVertical: 1, alignSelf: 'flex-start' },
  sectionHeader: { flexDirection: 'row', justifyContent: 'space-between', alignItems: 'center', marginTop: space.xl, marginBottom: space.md },
  chip: { borderWidth: 1, borderColor: colors.border, borderRadius: radius.pill, paddingHorizontal: space.md, paddingVertical: 6, marginRight: space.sm },
});
