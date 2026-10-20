import { useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { Animated, PanResponder, View } from 'react-native';
import { Ionicons } from '@expo/vector-icons';
import * as Haptics from 'expo-haptics';
import { colors } from '@/theme';

interface Props<T> {
  items: T[];
  keyOf: (t: T) => string;
  rowHeight: number;
  renderRow: (item: T, index: number, handle: ReactNode) => ReactNode;
  onMove: (from: number, to: number) => void;
  onDragStateChange?: (dragging: boolean) => void;
}

interface DragCallbacks {
  start: (index: number) => void;
  move: (index: number, dy: number) => void;
  end: () => void;
}

/** Alça (≡) de arrastar. Cria o PanResponder uma vez; os callbacks são sempre os mais recentes. */
function DragHandle({ index, height, count, callbacks, onStep }: { index: number; height: number; count: number; callbacks: DragCallbacks; onStep: (from: number, to: number) => void }) {
  // Padrão "latest ref": o PanResponder é criado uma vez (trocar no meio do gesto o cancelaria)
  // e lê sempre os callbacks mais recentes, atualizados após cada render.
  const latestRef = useRef({ index, callbacks });
  useLayoutEffect(() => {
    latestRef.current = { index, callbacks };
  });
  // eslint-disable-next-line react-hooks/refs -- o ref só é lido dentro dos handlers do gesto, nunca no render
  const [responder] = useState(() =>
    PanResponder.create({
      onStartShouldSetPanResponder: () => true,
      onMoveShouldSetPanResponderCapture: () => true,
      onPanResponderTerminationRequest: () => false,
      onPanResponderGrant: () => latestRef.current.callbacks.start(latestRef.current.index),
      onPanResponderMove: (_, g) => latestRef.current.callbacks.move(latestRef.current.index, g.dy),
      onPanResponderRelease: () => latestRef.current.callbacks.end(),
      onPanResponderTerminate: () => latestRef.current.callbacks.end(),
    }),
  );
  return (
    <View
      {...responder.panHandlers}
      style={{ width: 44, height, alignItems: 'center', justifyContent: 'center' }}
      accessibilityLabel="Arrastar para reordenar"
      accessibilityRole="adjustable"
      accessibilityActions={[
        { name: 'increment', label: 'Mover para baixo' },
        { name: 'decrement', label: 'Mover para cima' },
      ]}
      onAccessibilityAction={(e) => {
        // Alternativa acessível ao arrasto (leitores de tela).
        if (e.nativeEvent.actionName === 'increment' && index < count - 1) onStep(index, index + 1);
        if (e.nativeEvent.actionName === 'decrement' && index > 0) onStep(index, index - 1);
      }}
    >
      <Ionicons name="reorder-three" size={24} color={colors.textDim} />
    </View>
  );
}

/**
 * Lista com arrastar-e-soltar pela alça. Usa só PanResponder + Animated do
 * React Native (sem dependência nativa extra). Linhas de altura fixa tornam o
 * cálculo do destino O(1).
 */
export function ReorderableList<T>({ items, keyOf, rowHeight, renderRow, onMove, onDragStateChange }: Props<T>) {
  const [drag, setDrag] = useState<{ from: number; to: number } | null>(null);
  const [dy] = useState(() => new Animated.Value(0));
  const dragRef = useRef<{ from: number; to: number } | null>(null);

  const callbacks: DragCallbacks = {
    start: (index) => {
      dragRef.current = { from: index, to: index };
      dy.setValue(0);
      setDrag(dragRef.current);
      onDragStateChange?.(true);
      void Haptics.selectionAsync();
    },
    move: (index, d) => {
      dy.setValue(d);
      const to = Math.max(0, Math.min(items.length - 1, index + Math.round(d / rowHeight)));
      if (dragRef.current && to !== dragRef.current.to) {
        dragRef.current = { ...dragRef.current, to };
        setDrag(dragRef.current);
        void Haptics.selectionAsync();
      }
    },
    end: () => {
      const cur = dragRef.current;
      dragRef.current = null;
      setDrag(null);
      dy.setValue(0);
      onDragStateChange?.(false);
      if (cur && cur.from !== cur.to) onMove(cur.from, cur.to);
    },
  };

  return (
    <View>
      {items.map((item, i) => {
        let shift = 0;
        if (drag && i !== drag.from) {
          if (drag.from < drag.to && i > drag.from && i <= drag.to) shift = -rowHeight;
          if (drag.from > drag.to && i < drag.from && i >= drag.to) shift = rowHeight;
        }
        const isDragged = drag?.from === i;
        const handle = <DragHandle index={i} height={rowHeight} count={items.length} callbacks={callbacks} onStep={onMove} />;
        return (
          <Animated.View
            key={keyOf(item)}
            style={{
              height: rowHeight,
              zIndex: isDragged ? 10 : 0,
              backgroundColor: isDragged ? colors.surfaceHigh : 'transparent',
              transform: [{ translateY: isDragged ? dy : shift }],
            }}
          >
            {renderRow(item, i, handle)}
          </Animated.View>
        );
      })}
    </View>
  );
}
