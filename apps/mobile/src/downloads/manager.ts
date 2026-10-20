import { Directory, File, Paths } from 'expo-file-system';
import { create } from 'zustand';
import { pickSource, type AudioSource, type Track } from '@sonora/core';
import { addDownload, cacheTracks, listDownloads, removeDownload, type DownloadRow } from '@/db/local';
import { useLibrary } from '@/library/store';
import { deviceCaps } from '@/player/caps';

/**
 * Downloads seletivos para modo offline. Arquivos ficam no diretório de
 * documentos do app (não são apagados pelo sistema como o cache) no codec
 * original escolhido — FLAC hi-res é baixado sem transcodificação.
 */

interface DownloadState {
  files: Map<string, DownloadRow>;
  /** trackId → progresso 0..1 */
  active: Map<string, number>;
  totalBytes: number;
}

export const useDownloads = create<DownloadState>(() => ({ files: new Map(), active: new Map(), totalBytes: 0 }));

const dir = () => {
  const d = new Directory(Paths.document, 'offline');
  if (!d.exists) d.create({ intermediates: true });
  return d;
};

export async function initDownloads() {
  const rows = await listDownloads();
  const files = new Map<string, DownloadRow>();
  for (const r of rows) {
    if (new File(r.uri).exists) files.set(r.track_id, r);
    else await removeDownload(r.track_id);
  }
  useDownloads.setState({ files, totalBytes: [...files.values()].reduce((s, r) => s + r.bytes, 0) });
}

/** Fonte local, se a faixa estiver baixada (usada pelo player antes da rede). */
export function localFileFor(trackId: string): AudioSource | null {
  const r = useDownloads.getState().files.get(trackId);
  if (!r) return null;
  return { url: r.uri, codec: r.codec as AudioSource['codec'], bitrate: 0, sampleRateHz: 0, channels: 2 };
}

const queue: Track[] = [];
let running = 0;
const PARALLEL = 2;

export function download(tracks: Track[]) {
  const s = useDownloads.getState();
  for (const t of tracks) if (!s.files.has(t.id) && !s.active.has(t.id) && !queue.some((q) => q.id === t.id)) queue.push(t);
  void cacheTracks(tracks);
  pump();
}

function pump() {
  while (running < PARALLEL && queue.length) {
    const t = queue.shift()!;
    running++;
    void downloadOne(t).finally(() => {
      running--;
      pump();
    });
  }
}

async function downloadOne(t: Track) {
  const prefs = useLibrary.getState().audio;
  const src = pickSource(t.sources, prefs.downloadQuality, deviceCaps, prefs.preferSpatial);
  if (!src) return;
  setActive(t.id, 0);
  try {
    const ext = src.codec === 'alac' || src.codec === 'aac' ? 'm4a' : src.codec === 'eac3-joc' ? 'ec3' : src.codec;
    const dest = new File(dir(), `${t.id.replace(/[^a-zA-Z0-9_-]/g, '_')}.${ext}`);
    if (dest.exists) dest.delete();
    const file = await File.downloadFileAsync(src.url, dest);
    const row: DownloadRow = { track_id: t.id, uri: file.uri, codec: src.codec, bytes: file.size ?? 0, created_at: Date.now() };
    await addDownload(row);
    useDownloads.setState((s) => {
      const files = new Map(s.files).set(t.id, row);
      return { files, totalBytes: s.totalBytes + row.bytes };
    });
  } finally {
    setActive(t.id, null);
  }
}

function setActive(id: string, p: number | null) {
  useDownloads.setState((s) => {
    const active = new Map(s.active);
    if (p === null) active.delete(id);
    else active.set(id, p);
    return { active };
  });
}

export async function deleteDownload(trackId: string) {
  const r = useDownloads.getState().files.get(trackId);
  if (!r) return;
  const f = new File(r.uri);
  if (f.exists) f.delete();
  await removeDownload(trackId);
  useDownloads.setState((s) => {
    const files = new Map(s.files);
    files.delete(trackId);
    return { files, totalBytes: s.totalBytes - r.bytes };
  });
}

export async function deleteAllDownloads() {
  for (const id of [...useDownloads.getState().files.keys()]) await deleteDownload(id);
}
