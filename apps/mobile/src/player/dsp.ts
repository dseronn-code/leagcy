import { requireOptionalNativeModule } from 'expo';
import { create } from 'zustand';
import { kvGet, kvSet } from '@/db/local';

/**
 * Equalizador paramétrico de 10 bandas.
 *
 * O processamento acontece no módulo nativo `SonoraDsp` (Media3 AudioProcessor no
 * Android, AVAudioUnitEQ no iOS — ver docs/AUDIO_ENGINE.md). Em builds sem o
 * módulo (ex.: Expo Go) a UI funciona e as configurações são salvas, mas o
 * áudio passa sem processamento (bit-perfect).
 */
interface SonoraDspModule {
  setEqualizer(enabled: boolean, gainsDb: number[], preampDb: number): void;
  setBitPerfect(enabled: boolean): Promise<boolean>;
  getOutputInfo(): Promise<{ sampleRateHz: number; bitDepth: number; device: string; bitPerfect: boolean }>;
}

const native = requireOptionalNativeModule<SonoraDspModule>('SonoraDsp');

export const dspAvailable = native !== null;

export const EQ_BANDS_HZ = [31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000] as const;

export const EQ_PRESETS: Record<string, number[]> = {
  Plano: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
  Graves: [6, 5, 4, 2, 0, 0, 0, 0, 0, 0],
  Agudos: [0, 0, 0, 0, 0, 1, 2, 4, 5, 6],
  Vocal: [-2, -1, 0, 2, 4, 4, 3, 1, 0, -1],
  Acústico: [3, 3, 2, 1, 1, 1, 2, 3, 3, 2],
  Eletrônica: [5, 4, 1, 0, -2, 1, 0, 2, 4, 5],
  Clássica: [4, 3, 2, 1, -1, -1, 0, 2, 3, 4],
  'Fones (Harman)': [4, 3, 1, 0, -1, -1, 0, 2, 1, -2],
};

interface EqState {
  enabled: boolean;
  gains: number[];
  preset: string;
  /** Preamp negativo automático evita clipping quando alguma banda tem ganho positivo. */
  preampDb: number;
}

export const useEq = create<EqState>(() => ({ enabled: false, gains: EQ_PRESETS.Plano!, preset: 'Plano', preampDb: 0 }));

function apply(s: EqState) {
  native?.setEqualizer(s.enabled, s.gains, s.preampDb);
  void kvSet('eq', s);
}

export async function loadEq() {
  const saved = await kvGet<EqState>('eq');
  if (saved) useEq.setState(saved);
  apply(useEq.getState());
}

export function setEq(p: Partial<EqState>) {
  const next = { ...useEq.getState(), ...p };
  next.preampDb = next.enabled ? -Math.max(0, ...next.gains) : 0;
  useEq.setState(next);
  apply(next);
}

export async function setBitPerfect(enabled: boolean): Promise<boolean> {
  return native ? native.setBitPerfect(enabled) : false;
}

export async function outputInfo() {
  return native ? native.getOutputInfo() : null;
}
