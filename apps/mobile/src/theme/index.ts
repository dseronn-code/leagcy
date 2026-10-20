/** Tema escuro por padrão (OLED-friendly: preto real economiza bateria em telas AMOLED). */
export const colors = {
  bg: '#0A0A0C',
  surface: '#141418',
  surfaceHigh: '#1E1E24',
  border: '#2A2A32',
  text: '#F5F5F7',
  textDim: '#A1A1AA',
  textMuted: '#6B6B76',
  accent: '#7C5CFF',
  accentSoft: 'rgba(124, 92, 255, 0.16)',
  hires: '#F5C542',
  lossless: '#4FD1C5',
  danger: '#FF5C7A',
  success: '#3DDC97',
} as const;

export const space = { xs: 4, sm: 8, md: 12, lg: 16, xl: 24, xxl: 32 } as const;
export const radius = { sm: 6, md: 10, lg: 16, pill: 999 } as const;

export const type = {
  hero: { fontSize: 32, fontWeight: '800' as const, color: colors.text, letterSpacing: -0.5 },
  title: { fontSize: 22, fontWeight: '700' as const, color: colors.text },
  subtitle: { fontSize: 17, fontWeight: '600' as const, color: colors.text },
  body: { fontSize: 15, color: colors.text },
  caption: { fontSize: 13, color: colors.textDim },
  micro: { fontSize: 11, color: colors.textMuted, letterSpacing: 0.4 },
};

/** Alvo mínimo de toque (acessibilidade WCAG / Material). */
export const HIT = 44;
