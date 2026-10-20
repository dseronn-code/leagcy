import { Text, View } from 'react-native';
import Svg, { Rect, Text as SvgText } from 'react-native-svg';
import { colors, space, type } from '@/theme';

/** Gráfico de barras simples e acessível (minutos por período). */
export function BarChart({ data, height = 160, width }: { data: { label: string; value: number }[]; height?: number; width: number }) {
  const max = Math.max(1, ...data.map((d) => d.value));
  const gap = 4;
  const bw = data.length ? Math.max(2, (width - gap * (data.length - 1)) / data.length) : 0;
  const chartH = height - 18;
  const showEvery = Math.ceil(data.length / 7);
  const total = data.reduce((s, d) => s + d.value, 0);
  return (
    <View accessible accessibilityLabel={`Gráfico: total de ${total} minutos em ${data.length} períodos. Máximo ${max} minutos.`}>
      <Svg width={width} height={height}>
        {data.map((d, i) => {
          const h = (d.value / max) * (chartH - 4);
          return (
            <Rect key={d.label} x={i * (bw + gap)} y={chartH - h} width={bw} height={Math.max(h, 1)} rx={Math.min(3, bw / 2)} fill={d.value === max ? colors.accent : colors.surfaceHigh} />
          );
        })}
        {data.map((d, i) =>
          i % showEvery === 0 ? (
            <SvgText key={`l-${d.label}`} x={i * (bw + gap) + bw / 2} y={height - 4} fontSize={10} fill={colors.textMuted} textAnchor="middle">
              {d.label}
            </SvgText>
          ) : null,
        )}
      </Svg>
    </View>
  );
}

const DAYS = ['D', 'S', 'T', 'Q', 'Q', 'S', 'S'];

/** Relógio de escuta: dia da semana × hora. */
export function Heatmap({ grid, width }: { grid: number[][]; width: number }) {
  const max = Math.max(1, ...grid.flat());
  const cell = (width - 20) / 24;
  return (
    <View accessible accessibilityLabel="Mapa de calor de horários de escuta">
      {grid.map((row, d) => (
        <View key={d} style={{ flexDirection: 'row', alignItems: 'center' }}>
          <Text style={[type.micro, { width: 20 }]}>{DAYS[d]}</Text>
          {row.map((v, h) => (
            <View key={h} style={{ width: cell - 2, height: cell - 2, margin: 1, borderRadius: 2, backgroundColor: colors.accent, opacity: v === 0 ? 0.06 : 0.15 + 0.85 * (v / max) }} />
          ))}
        </View>
      ))}
      <View style={{ flexDirection: 'row', justifyContent: 'space-between', marginLeft: 20, marginTop: space.xs }}>
        {['0h', '6h', '12h', '18h', '23h'].map((l) => (
          <Text key={l} style={type.micro}>
            {l}
          </Text>
        ))}
      </View>
    </View>
  );
}
