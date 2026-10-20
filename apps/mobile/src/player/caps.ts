import { Platform } from 'react-native';
import type { DeviceCaps } from '@sonora/core';

/**
 * Capacidades de decodificação. ExoPlayer (Android) e AVPlayer (iOS 11+)
 * decodificam FLAC/ALAC até 32-bit/384 kHz; a taxa entregue ao conversor
 * depende do DAC/rota (ver dsp.outputInfo e docs/AUDIO_ENGINE.md).
 */
export const deviceCaps: DeviceCaps = {
  maxSampleRateHz: 384_000,
  maxBitDepth: 32,
  supportsFlac: true,
  supportsAlac: true,
  // E-AC-3 JOC (Atmos): Android com decoder do sistema; iOS via AVPlayer.
  supportsSpatial: Platform.OS === 'ios' || Platform.OS === 'android',
};
