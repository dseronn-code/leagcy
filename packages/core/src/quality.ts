import type { AudioSource, StreamQuality } from './types';

/**
 * Escolhe a melhor fonte de áudio permitida pela preferência do usuário e
 * pelas capacidades do dispositivo. Nunca faz upsampling "falso": se a faixa
 * só existe em 16/44.1, é isso que toca.
 */

export interface DeviceCaps {
  maxSampleRateHz: number;
  maxBitDepth: number;
  supportsFlac: boolean;
  supportsAlac: boolean;
  supportsSpatial: boolean;
}

const LOSSLESS = new Set(['flac', 'alac', 'wav']);

export function isLossless(s: AudioSource): boolean {
  return LOSSLESS.has(s.codec);
}

export function isHiRes(s: AudioSource): boolean {
  return isLossless(s) && ((s.bitDepth ?? 16) > 16 || s.sampleRateHz > 48_000);
}

function allowed(s: AudioSource, q: StreamQuality): boolean {
  switch (q) {
    case 'data-saver':
      return !isLossless(s) && s.bitrate <= 128;
    case 'high':
      return !isLossless(s) && s.bitrate <= 320;
    case 'lossless':
      return !isHiRes(s);
    case 'hires':
    case 'max':
      return true;
  }
}

function playable(s: AudioSource, caps: DeviceCaps): boolean {
  if (s.codec === 'flac' && !caps.supportsFlac) return false;
  if (s.codec === 'alac' && !caps.supportsAlac) return false;
  if (s.spatial && !caps.supportsSpatial) return false;
  if (s.sampleRateHz > caps.maxSampleRateHz) return false;
  if ((s.bitDepth ?? 16) > caps.maxBitDepth) return false;
  return true;
}

/** Pontuação de fidelidade: lossless > lossy; depois bit depth, sample rate e bitrate. */
function fidelity(s: AudioSource): number {
  return (isLossless(s) ? 1e9 : 0) + (s.bitDepth ?? 16) * 1e7 + s.sampleRateHz * 10 + s.bitrate;
}

export function pickSource(sources: AudioSource[], quality: StreamQuality, caps: DeviceCaps, preferSpatial = false): AudioSource | null {
  const candidates = sources.filter((s) => playable(s, caps));
  if (preferSpatial) {
    const spatial = candidates.find((s) => s.spatial);
    if (spatial) return spatial;
  }
  const stereo = candidates.filter((s) => !s.spatial);
  const ok = stereo.filter((s) => allowed(s, quality)).sort((a, b) => fidelity(b) - fidelity(a));
  if (ok[0]) return ok[0];
  // Nada dentro do limite (ex.: só existe FLAC e o usuário está em economia): pega a menor disponível.
  return stereo.sort((a, b) => fidelity(a) - fidelity(b))[0] ?? candidates[0] ?? null;
}

/** Rótulo curto para a UI: "24-bit/192 kHz FLAC", "Dolby Atmos", "AAC 256". */
export function qualityLabel(s: AudioSource): string {
  if (s.spatial) return 'Dolby Atmos';
  if (isLossless(s)) {
    const khz = s.sampleRateHz / 1000;
    return `${s.bitDepth ?? 16}-bit/${Number.isInteger(khz) ? khz : khz.toFixed(1)} kHz ${s.codec.toUpperCase()}`;
  }
  return `${s.codec.toUpperCase()} ${s.bitrate}`;
}

/**
 * Ganho linear de normalização (ReplayGain). Limita pelo pico para nunca
 * clipar — mantém a fidelidade em vez de comprimir.
 */
export function normalizationGain(s: AudioSource, mode: 'off' | 'track' | 'album' | 'auto', targetLufs = -14, inAlbumContext = false): number {
  if (mode === 'off') return 1;
  const useAlbum = mode === 'album' || (mode === 'auto' && inAlbumContext);
  const gainDb = (useAlbum ? s.albumGainDb : undefined) ?? s.trackGainDb;
  if (gainDb === undefined) return 1;
  // ReplayGain 2.0 tem referência de -18 LUFS; ajusta para o alvo escolhido.
  const linear = Math.pow(10, (gainDb + (targetLufs + 18)) / 20);
  const peak = s.trackPeak ?? 1;
  return Math.min(linear, 1 / Math.max(peak, 1e-6));
}
