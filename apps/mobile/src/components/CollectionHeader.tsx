import { type ReactNode } from 'react';
import { Text, View } from 'react-native';
import { router } from 'expo-router';
import { LinearGradient } from 'expo-linear-gradient';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { colors, space, type } from '@/theme';
import { Artwork, IconButton } from './ui';

/** Cabeçalho comum de álbum/playlist/mix: capa, título, metadados e ações. */
export function CollectionHeader({ artwork, title, subtitle, meta, actions, round }: { artwork?: string; title: string; subtitle?: string; meta?: string; actions: ReactNode; round?: boolean }) {
  const insets = useSafeAreaInsets();
  return (
    <LinearGradient colors={['#2A1E66', colors.bg]} style={{ paddingTop: insets.top, paddingHorizontal: space.lg, paddingBottom: space.md }}>
      <IconButton name="chevron-back" label="Voltar" onPress={() => router.back()} />
      <View style={{ alignItems: 'center', marginTop: space.sm }}>
        <Artwork uri={artwork} size={220} round={round} />
      </View>
      <Text style={[type.title, { marginTop: space.lg }]} accessibilityRole="header">
        {title}
      </Text>
      {subtitle && <Text style={[type.body, { color: colors.textDim, marginTop: 2 }]}>{subtitle}</Text>}
      {meta && <Text style={[type.caption, { marginTop: 2 }]}>{meta}</Text>}
      <View style={{ flexDirection: 'row', alignItems: 'center', marginTop: space.md }}>{actions}</View>
    </LinearGradient>
  );
}

export function PlayButtons({ onPlay, onShuffle }: { onPlay: () => void; onShuffle: () => void }) {
  return (
    <View style={{ flexDirection: 'row', alignItems: 'center', marginLeft: 'auto' }}>
      <IconButton name="shuffle" size={26} label="Tocar em ordem aleatória" onPress={onShuffle} />
      <View style={{ width: 56, height: 56, borderRadius: 28, backgroundColor: colors.accent, alignItems: 'center', justifyContent: 'center' }}>
        <IconButton name="play" size={28} label="Tocar" onPress={onPlay} />
      </View>
    </View>
  );
}
