import { createAudioPlaylist, setAudioModeAsync, type AudioPlaylist, type AudioPlaylistStatus } from 'expo-audio';
import * as Crypto from 'expo-crypto';
import * as Network from 'expo-network';
import { create } from 'zustand';
import {
  addToQueue as qAdd,
  createQueue,
  currentTrack,
  cycleRepeat,
  jumpToContextIndex,
  ListenClock,
  moveInUpcomingContext,
  moveInUserQueue,
  next as qNext,
  normalizationGain,
  pickSource,
  playNext as qPlayNext,
  previous as qPrev,
  removeFromUserQueue,
  setShuffle,
  upNext,
  type AudioSource,
  type PlayEvent,
  type PlaySource,
  type QueueState,
  type Track,
} from '@sonora/core';
import { api, ApiError } from '@/api/client';
import { cacheTracks, insertPlay, kvGet, kvSet } from '@/db/local';
import { localFileFor } from '@/downloads/manager';
import { useLibrary } from '@/library/store';
import { deviceCaps } from './caps';

/**
 * Motor de reprodução.
 *
 * - A fila lógica (contexto, fila do usuário, shuffle, repeat) vive no
 *   @sonora/core e é a fonte da verdade.
 * - O expo-audio `AudioPlaylist` (ExoPlayer no Android, AVQueuePlayer no iOS)
 *   recebe uma JANELA com a faixa atual + as próximas 2. Como as próximas já
 *   estão pré-carregadas no player nativo, a transição é gapless e não depende
 *   do JS (funciona com a tela bloqueada e com o JS ocupado).
 * - Cada mudança de faixa gera um PlayEvent com o tempo REAL ouvido.
 */

const LOOKAHEAD = 2;

export interface PlayerState {
  queue: QueueState | null;
  tracks: Record<string, Track>;
  current: Track | null;
  source: AudioSource | null;
  playing: boolean;
  buffering: boolean;
  positionSec: number;
  durationSec: number;
  rate: number;
  context: PlaySource | null;
  contextTitle: string | null;
  /** Ao acabar a fila, continua com rádio baseado no que tocou. */
  autoplay: boolean;
  sleepAt: number | null;
  /** Última faixa pulada por indisponibilidade (sem assinatura, fora do território...). */
  unavailable: { title: string; reason: string } | null;
}

export const usePlayer = create<PlayerState>(() => ({
  queue: null,
  tracks: {},
  current: null,
  source: null,
  playing: false,
  buffering: false,
  positionSec: 0,
  durationSec: 0,
  rate: 1,
  context: null,
  contextTitle: null,
  autoplay: true,
  sleepAt: null,
  unavailable: null,
}));

let native: AudioPlaylist | null = null;
/** IDs na janela nativa, alinhados com os índices do AudioPlaylist. */
let nativeIds: string[] = [];
let nativeIndex = 0;
const clock = new ListenClock();
let currentStartedAt = 0;
let sleepTimer: ReturnType<typeof setTimeout> | null = null;
let onCellular = false;
let fetchingRadio = false;
/** Invalida janelas em construção quando o usuário troca de faixa de novo no meio do await. */
let windowGen = 0;
/** URLs assinadas de catálogo licenciado expiram rápido; reaproveitamos por poucos minutos. */
const signedCache = new Map<string, { sources: AudioSource[]; at: number }>();
const SIGNED_TTL_MS = 4 * 60_000;
/** Faixas que o servidor recusou nesta sessão (não tenta de novo a cada reconciliação). */
const refused = new Map<string, string>();

const get = () => usePlayer.getState();
const set = (p: Partial<PlayerState>) => usePlayer.setState(p);

export async function initPlayer() {
  await setAudioModeAsync({ playsInSilentMode: true, shouldPlayInBackground: true, interruptionMode: 'doNotMix' });
  native = createAudioPlaylist({ sources: [], updateInterval: 500, loop: 'none' });
  native.addListener('playlistStatusUpdate', onStatus);
  native.addListener('trackChanged', ({ previousIndex, currentIndex }) => void onNativeTrackChanged(previousIndex, currentIndex));

  const net = await Network.getNetworkStateAsync();
  onCellular = net.type === Network.NetworkStateType.CELLULAR;
  Network.addNetworkStateListener((s) => {
    onCellular = s.type === Network.NetworkStateType.CELLULAR;
  });

  // Restaura a última sessão (fila e faixa), pausada.
  const saved = await kvGet<{ queue: QueueState; tracks: Record<string, Track>; context: PlaySource | null; contextTitle: string | null }>('player');
  if (saved?.queue) {
    set({ ...saved });
    await loadWindow(false);
  }
}

function onStatus(s: AudioPlaylistStatus) {
  const wasPlaying = get().playing;
  if (s.playing && !wasPlaying) clock.play();
  if (!s.playing && wasPlaying) clock.pause();
  set({ playing: s.playing, buffering: s.isBuffering, positionSec: s.currentTime, durationSec: s.duration || (get().current?.durationMs ?? 0) / 1000 });
  const q = get().queue;
  if (s.didJustFinish && q && s.currentIndex === nativeIds.length - 1 && !upNext(q, 1).length) void onQueueEnded();
}

/** O player nativo avançou sozinho (fim da faixa ou botão da tela de bloqueio/fone/carro). */
async function onNativeTrackChanged(prev: number, cur: number) {
  if (cur === nativeIndex) return; // mudança iniciada por nós
  logPlay(cur < prev ? 'manual' : 'auto');
  let q = get().queue;
  if (!q) return;
  if (cur > prev) {
    // Avança a fila lógica até a faixa que o nativo está tocando (faixas indisponíveis ficaram fora da janela).
    const target = nativeIds[cur];
    for (let i = 0; i < 50 && q && currentTrack(q) !== target; i++) q = qNext(q, false) ?? q;
  } else q = qPrev(q);
  nativeIndex = cur;
  commitQueue(q);
  await reconcileLookahead();
}

function resolveSource(t: Track): AudioSource | null {
  const offline = localFileFor(t.id);
  if (offline) return offline;
  const prefs = useLibrary.getState().audio;
  return pickSource(t.sources, onCellular ? prefs.cellularQuality : prefs.wifiQuality, deviceCaps, prefs.preferSpatial);
}

/**
 * Fonte tocável para o player nativo. Arquivo baixado tem prioridade; faixas de
 * catálogo licenciado pedem URL assinada ao servidor (que valida assinatura e território).
 */
async function nativeSourceFor(id: string): Promise<{ uri: string; name: string } | null> {
  const t = get().tracks[id];
  if (!t || refused.has(id)) return null;
  const offline = localFileFor(t.id);
  if (offline) return { uri: offline.url, name: t.title };
  if (!t.sources.some((s) => s.requiresAuthorization)) {
    const src = resolveSource(t);
    return src ? { uri: src.url, name: t.title } : null;
  }
  let cached = signedCache.get(id);
  if (!cached || Date.now() - cached.at > SIGNED_TTL_MS) {
    try {
      const { sources } = await api.stream(id, useLibrary.getState().deviceId);
      cached = { sources, at: Date.now() };
      signedCache.set(id, cached);
    } catch (e) {
      const reason = e instanceof ApiError && e.status === 403 ? e.message : 'offline';
      if (reason !== 'offline') refused.set(id, reason);
      set({ unavailable: { title: t.title, reason } });
      return null;
    }
  }
  const prefs = useLibrary.getState().audio;
  const src = pickSource(cached.sources, onCellular ? prefs.cellularQuality : prefs.wifiQuality, deviceCaps, prefs.preferSpatial);
  return src ? { uri: src.url, name: t.title } : null;
}

function applyTrackSettings() {
  if (!native) return;
  const t = get().current;
  const src = get().source;
  const prefs = useLibrary.getState().audio;
  const inAlbum = get().context?.type === 'album';
  // Volume ≤ 1: só atenuamos (nunca boost digital que poderia clipar).
  native.volume = src ? Math.min(1, normalizationGain(src, prefs.normalization, prefs.normalizationTargetLufs, inAlbum)) : 1;
  native.playbackRate = get().rate;
  native.loop = get().queue?.repeat === 'one' ? 'single' : 'none';
  if (t) {
    native.updateLockScreenMetadata({ title: t.title, artist: t.artists.map((a) => a.name).join(', '), albumTitle: t.albumTitle, artworkUrl: t.artworkUrl });
  }
}

function commitQueue(q: QueueState) {
  const id = currentTrack(q);
  const t = id ? get().tracks[id] ?? null : null;
  if (t?.id !== get().current?.id) {
    clock.reset();
    if (get().playing) clock.play();
    currentStartedAt = Date.now();
  }
  set({ queue: q, current: t, source: t ? resolveSource(t) : null });
  applyTrackSettings();
  void kvSet('player', { queue: q, tracks: pruneTracks(q), context: get().context, contextTitle: get().contextTitle });
  if (upNext(q, LOOKAHEAD + 1).length <= LOOKAHEAD) void maybeExtendWithRadio();
}

function pruneTracks(q: QueueState): Record<string, Track> {
  const keep = new Set([...q.context, ...q.userQueue, ...(q.playingFromUserQueue ? [q.playingFromUserQueue] : [])]);
  return Object.fromEntries(Object.entries(get().tracks).filter(([id]) => keep.has(id)));
}

/** Recria a janela nativa a partir da faixa atual (usado em saltos manuais). */
async function loadWindow(autoplay: boolean) {
  if (!native) return;
  const gen = ++windowGen;
  // Pula faixas indisponíveis no início (sem assinatura, fora do território), como os apps de referência.
  let first: { uri: string; name: string } | null = null;
  for (let attempts = 0; attempts < 20; attempts++) {
    const q = get().queue;
    const cur = q && currentTrack(q);
    if (!cur) break;
    first = await nativeSourceFor(cur);
    if (gen !== windowGen) return;
    if (first) break;
    const n = qNext(q, false);
    if (!n) break;
    commitQueue(n);
  }
  native.clear();
  nativeIds = [];
  nativeIndex = 0;
  const q = get().queue;
  const cur = q && currentTrack(q);
  if (!cur || !first) return;
  native.add(first);
  nativeIds.push(cur);
  for (const id of upNext(q, LOOKAHEAD)) {
    const src = await nativeSourceFor(id);
    if (gen !== windowGen) return;
    if (!src) continue;
    native.add(src);
    nativeIds.push(id);
  }
  const t = get().current;
  native.setActiveForLockScreen(true, t ? { title: t.title, artist: t.artists[0]?.name, albumTitle: t.albumTitle, artworkUrl: t.artworkUrl } : undefined, {
    showNextTrack: true,
    showPreviousTrack: true,
    showSeekForward: false,
    showSeekBackward: false,
  });
  applyTrackSettings();
  if (autoplay) native.play();
}

/** Garante que as próximas faixas na janela nativa batem com a fila lógica. */
async function reconcileLookahead() {
  if (!native) return;
  const gen = windowGen;
  const want = upNext(get().queue!, LOOKAHEAD + refused.size).filter((id) => !refused.has(id)).slice(0, LOOKAHEAD);
  const have = nativeIds.slice(nativeIndex + 1);
  if (want.length === have.length && want.every((id, i) => id === have[i])) return;
  for (let i = nativeIds.length - 1; i > nativeIndex; i--) native.remove(i);
  nativeIds = nativeIds.slice(0, nativeIndex + 1);
  for (const id of want) {
    const src = await nativeSourceFor(id);
    if (gen !== windowGen) return;
    if (!src) continue;
    native.add(src);
    nativeIds.push(id);
  }
}

/** Fecha a escuta atual como PlayEvent (base de estatísticas e recomendações). */
function logPlay(reason: 'auto' | 'manual' | 'stop') {
  const t = get().current;
  const listenedMs = Math.round(clock.listenedMs);
  clock.reset();
  if (get().playing) clock.play();
  if (!t || listenedMs < 1000) return;
  const minQualified = Math.min(30_000, t.durationMs * 0.5);
  const e: PlayEvent = {
    id: Crypto.randomUUID(),
    trackId: t.id,
    artistIds: t.artists.map((a) => a.id),
    genres: t.genres,
    startedAt: currentStartedAt || Date.now() - listenedMs,
    listenedMs,
    trackDurationMs: t.durationMs,
    skipped: reason === 'manual' && listenedMs < minQualified,
    source: get().context ?? { type: 'queue' },
    deviceId: useLibrary.getState().deviceId,
    tzOffsetMin: -new Date().getTimezoneOffset(),
  };
  currentStartedAt = Date.now();
  void insertPlay(e);
}

function remember(tracks: Track[]) {
  set({ tracks: { ...get().tracks, ...Object.fromEntries(tracks.map((t) => [t.id, t])) } });
  void cacheTracks(tracks);
}

// ---------------- API pública ----------------

/** Toca uma lista (álbum, playlist, busca...) a partir de um índice. */
export async function playTracks(tracks: Track[], startIndex: number, context: PlaySource, contextTitle: string, opts: { shuffle?: boolean } = {}) {
  logPlay('manual');
  remember(tracks);
  const prev = get().queue;
  const q = createQueue(
    tracks.map((t) => t.id),
    startIndex,
    { shuffle: opts.shuffle ?? prev?.shuffle ?? false, repeat: prev?.repeat ?? 'off', seed: Date.now() & 0xffff },
  );
  set({ context, contextTitle });
  commitQueue(q);
  await loadWindow(true);
}

export function togglePlay() {
  if (!native) return;
  if (get().playing) native.pause();
  else native.play();
}

export async function skipNext() {
  const q = get().queue;
  if (!q) return;
  const n = qNext(q, false);
  if (!n) return;
  logPlay('manual');
  const nextId = currentTrack(n);
  commitQueue(n);
  if (native && nativeIds[nativeIndex + 1] === nextId) {
    nativeIndex += 1;
    native.next(); // já pré-carregada: troca instantânea
    await reconcileLookahead();
  } else await loadWindow(true);
}

export async function skipPrevious() {
  if (!native || !get().queue) return;
  // Comportamento padrão: após 3 s, "anterior" reinicia a faixa.
  if (get().positionSec > 3) return void native.seekTo(0);
  logPlay('manual');
  commitQueue(qPrev(get().queue!));
  await loadWindow(true);
}

export function seek(sec: number) {
  void native?.seekTo(Math.max(0, sec));
}

export async function jumpTo(contextIndex: number) {
  const q = get().queue;
  if (!q) return;
  logPlay('manual');
  commitQueue(jumpToContextIndex(q, contextIndex));
  await loadWindow(true);
}

/** Toca um item da fila do usuário imediatamente. */
export async function playFromUserQueue(index: number) {
  const q = get().queue;
  const id = q?.userQueue[index];
  if (!q || !id) return;
  logPlay('manual');
  commitQueue({ ...q, userQueue: q.userQueue.filter((_, i) => i !== index), playingFromUserQueue: id });
  await loadWindow(true);
}

export async function playNext(tracks: Track[]) {
  remember(tracks);
  if (!get().queue) return playTracks(tracks, 0, { type: 'queue' }, 'Fila');
  commitQueue(qPlayNext(get().queue!, tracks.map((t) => t.id)));
  await reconcileLookahead();
}

export async function addToQueue(tracks: Track[]) {
  remember(tracks);
  if (!get().queue) return playTracks(tracks, 0, { type: 'queue' }, 'Fila');
  commitQueue(qAdd(get().queue!, tracks.map((t) => t.id)));
  await reconcileLookahead();
}

export async function removeQueued(index: number) {
  commitQueue(removeFromUserQueue(get().queue!, index));
  await reconcileLookahead();
}

export async function reorderQueue(section: 'user' | 'context', from: number, to: number) {
  const q = get().queue!;
  commitQueue(section === 'user' ? moveInUserQueue(q, from, to) : moveInUpcomingContext(q, from, to));
  await reconcileLookahead();
}

export async function toggleShuffle() {
  const q = get().queue;
  if (!q) return;
  commitQueue(setShuffle(q, !q.shuffle));
  await reconcileLookahead();
}

export function toggleRepeat() {
  const q = get().queue;
  if (q) commitQueue(cycleRepeat(q));
}

export function setRate(rate: number) {
  set({ rate });
  if (native) native.playbackRate = rate;
}

export function setAutoplay(on: boolean) {
  set({ autoplay: on });
}

/** Timer de sono: pausa após N minutos (ou no fim da faixa se `endOfTrack`). */
export function setSleepTimer(minutes: number | null) {
  if (sleepTimer) clearTimeout(sleepTimer);
  sleepTimer = null;
  if (minutes === null) return set({ sleepAt: null });
  const at = Date.now() + minutes * 60_000;
  sleepTimer = setTimeout(() => {
    native?.pause();
    set({ sleepAt: null });
  }, minutes * 60_000);
  set({ sleepAt: at });
}

/** Rádio a partir de uma faixa: substitui a fila por uma estação infinita. */
export async function startRadio(seed: Track) {
  const { tracks } = await api.radio(seed.id);
  await playTracks([seed, ...tracks], 0, { type: 'radio', seed: seed.id }, `Rádio de ${seed.title}`, { shuffle: false });
}

async function maybeExtendWithRadio() {
  const { queue, autoplay, current } = get();
  if (!autoplay || !queue || !current || fetchingRadio || queue.repeat !== 'off') return;
  fetchingRadio = true;
  try {
    const { tracks } = await api.radio(current.id, queue.context);
    if (!tracks.length) return;
    remember(tracks);
    const q = get().queue!;
    const base = q.context.length;
    commitQueue({ ...q, context: [...q.context, ...tracks.map((t) => t.id)], order: [...q.order, ...tracks.map((_, i) => base + i)] });
    await reconcileLookahead();
  } catch {
    // offline: simplesmente termina a fila
  } finally {
    fetchingRadio = false;
  }
}

async function onQueueEnded() {
  logPlay('auto');
  set({ playing: false });
}

/** Re-resolve fontes quando a preferência de qualidade ou a rede mudam. */
export async function refreshSources() {
  const pos = get().positionSec;
  const wasPlaying = get().playing;
  await loadWindow(wasPlaying);
  if (pos > 0) seek(pos);
}
