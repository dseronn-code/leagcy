import { create } from 'zustand';
import * as Crypto from 'expo-crypto';
import {
  applyOps,
  compareEntries,
  HybridClock,
  keyBetween,
  keysBetween,
  materialize,
  type AudioPreferences,
  type ChangeOp,
  type EntityType,
  type Playlist,
  type PlaylistEntry,
  type SmartRules,
  type Store,
  type Track,
} from '@sonora/core';
import { cacheTracks, enqueueOps, kvGet, kvSet, loadOps, saveOps } from '@/db/local';

/**
 * Biblioteca do usuário, derivada de um log de ChangeOps (ver core/sync.ts).
 * Toda ação gera ops locais → aplica na hora (UI otimista, funciona offline)
 * → grava no outbox → o SyncEngine envia quando houver rede.
 */

export interface SavedAlbum {
  id: string;
  title: string;
  artist: string;
  artworkUrl?: string;
  savedAt: number;
}

export interface FollowedArtist {
  id: string;
  name: string;
  imageUrl?: string;
}

export const DEFAULT_AUDIO: AudioPreferences = {
  wifiQuality: 'max',
  cellularQuality: 'lossless',
  downloadQuality: 'max',
  normalization: 'auto',
  normalizationTargetLufs: -14,
  gapless: true,
  crossfadeMs: 0,
  preferSpatial: false,
  bitPerfectUsb: true,
  playbackSpeed: 1,
};

interface LibraryState {
  ready: boolean;
  userId: string | null;
  deviceId: string;
  likes: Map<string, number>;
  playlists: Playlist[];
  savedAlbums: SavedAlbum[];
  followedArtists: FollowedArtist[];
  followedPlaylists: Set<string>;
  audio: AudioPreferences;
  /** Incrementado a cada mudança local, para o SyncEngine agendar envio. */
  dirty: number;
}

let clock: HybridClock;
let store: Store = new Map();
const collaborativeIds = new Set<string>();

const useLibraryStore = create<LibraryState>(() => ({
  ready: false,
  userId: null,
  deviceId: '',
  likes: new Map(),
  playlists: [],
  savedAlbums: [],
  followedArtists: [],
  followedPlaylists: new Set(),
  audio: DEFAULT_AUDIO,
  dirty: 0,
}));

export const useLibrary = useLibraryStore;

function derive(): Partial<LibraryState> {
  const likes = new Map<string, number>();
  for (const l of materialize(store, 'like')) likes.set(l.id, (l.at as number) ?? 0);

  const entriesByPlaylist = new Map<string, PlaylistEntry[]>();
  for (const e of materialize(store, 'playlistEntry')) {
    const [pid, entryId] = e.id.split('/') as [string, string];
    const list = entriesByPlaylist.get(pid) ?? [];
    list.push({ entryId, trackId: e.trackId as string, position: e.position as string, addedBy: e.addedBy as string, addedAt: e.addedAt as number });
    entriesByPlaylist.set(pid, list);
  }

  collaborativeIds.clear();
  const playlists: Playlist[] = materialize(store, 'playlist')
    .filter((p) => p.name !== undefined)
    .map((p) => {
      const kind = (p.kind as Playlist['kind']) ?? 'user';
      if (kind === 'collaborative') collaborativeIds.add(p.id);
      return {
        id: p.id,
        ownerId: (p.ownerId as string) ?? '',
        name: p.name as string,
        description: p.description as string | undefined,
        artworkUrl: p.artworkUrl as string | undefined,
        kind,
        collaboratorIds: [],
        isPublic: Boolean(p.isPublic),
        smartRules: p.smartRules as SmartRules | undefined,
        folderId: p.folderId as string | undefined,
        entries: (entriesByPlaylist.get(p.id) ?? []).sort(compareEntries),
        updatedAt: (p.updatedAt as number) ?? 0,
      };
    })
    .sort((a, b) => b.updatedAt - a.updatedAt);

  const savedAlbums = materialize(store, 'savedAlbum').map((a) => ({ id: a.id, title: a.title as string, artist: a.artist as string, artworkUrl: a.artworkUrl as string | undefined, savedAt: (a.savedAt as number) ?? 0 }));
  const followedArtists = materialize(store, 'followArtist').map((a) => ({ id: a.id, name: a.name as string, imageUrl: a.imageUrl as string | undefined }));
  const followedPlaylists = new Set(materialize(store, 'followPlaylist').map((p) => p.id));
  const settings = materialize(store, 'settings').find((s) => s.id === 'audio');
  const audio = { ...DEFAULT_AUDIO, ...(settings ? (Object.fromEntries(Object.entries(settings).filter(([k]) => k !== 'id')) as Partial<AudioPreferences>) : {}) };

  return { likes, playlists, savedAlbums: savedAlbums.sort((a, b) => b.savedAt - a.savedAt), followedArtists, followedPlaylists, audio };
}

export async function initLibrary(userId: string) {
  let deviceId = await kvGet<string>('deviceId');
  if (!deviceId) {
    deviceId = Crypto.randomUUID();
    await kvSet('deviceId', deviceId);
  }
  clock = new HybridClock(deviceId);
  const ops = await loadOps();
  store = applyOps(new Map(), ops);
  for (const op of ops) clock.receive(op.hlc);
  useLibraryStore.setState({ ...derive(), userId, deviceId, ready: true });
}

/** Aplica ops vindas do servidor (pull). */
export async function applyRemote(ops: ChangeOp[]) {
  if (!ops.length) return;
  for (const op of ops) clock.receive(op.hlc);
  store = applyOps(store, ops);
  await saveOps(ops);
  useLibraryStore.setState(derive());
}

function scopeFor(entity: EntityType, entityId: string): string {
  const { userId } = useLibraryStore.getState();
  const playlistId = entity === 'playlist' ? entityId : entity === 'playlistEntry' ? entityId.split('/')[0]! : null;
  if (playlistId && collaborativeIds.has(playlistId)) return `playlist:${playlistId}`;
  return `user:${userId}`;
}

async function commit(changes: { entity: EntityType; entityId: string; fields: Record<string, unknown> }[]) {
  const { deviceId } = useLibraryStore.getState();
  const byScope = new Map<string, ChangeOp[]>();
  const all: ChangeOp[] = [];
  for (const c of changes) {
    const scope = scopeFor(c.entity, c.entityId);
    for (const [field, value] of Object.entries(c.fields)) {
      const op: ChangeOp = { entity: c.entity, entityId: c.entityId, field, value, hlc: clock.tick(), deviceId };
      all.push(op);
      byScope.set(scope, [...(byScope.get(scope) ?? []), op]);
    }
  }
  store = applyOps(store, all);
  useLibraryStore.setState((s) => ({ ...derive(), dirty: s.dirty + 1 }));
  await saveOps(all);
  for (const [scope, ops] of byScope) await enqueueOps(scope, ops);
}

// ---------- Curtidas / biblioteca ----------
export function toggleLike(t: Track) {
  const liked = useLibraryStore.getState().likes.has(t.id);
  void cacheTracks([t]);
  return commit([{ entity: 'like', entityId: t.id, fields: { $exists: !liked, at: Date.now() } }]);
}

export function toggleSaveAlbum(a: { id: string; title: string; artist: string; artworkUrl?: string }) {
  const saved = useLibraryStore.getState().savedAlbums.some((x) => x.id === a.id);
  return commit([{ entity: 'savedAlbum', entityId: a.id, fields: saved ? { $exists: false } : { $exists: true, title: a.title, artist: a.artist, artworkUrl: a.artworkUrl, savedAt: Date.now() } }]);
}

export function toggleFollowArtist(a: FollowedArtist) {
  const following = useLibraryStore.getState().followedArtists.some((x) => x.id === a.id);
  return commit([{ entity: 'followArtist', entityId: a.id, fields: following ? { $exists: false } : { $exists: true, name: a.name, imageUrl: a.imageUrl } }]);
}

export function toggleFollowPlaylist(id: string) {
  const following = useLibraryStore.getState().followedPlaylists.has(id);
  return commit([{ entity: 'followPlaylist', entityId: id, fields: { $exists: !following } }]);
}

export function setAudioPrefs(p: Partial<AudioPreferences>) {
  return commit([{ entity: 'settings', entityId: 'audio', fields: p }]);
}

// ---------- Playlists ----------
export async function createPlaylist(name: string, opts: { kind?: Playlist['kind']; smartRules?: SmartRules; tracks?: Track[] } = {}): Promise<string> {
  const id = Crypto.randomUUID();
  const kind = opts.kind ?? 'user';
  if (kind === 'collaborative') collaborativeIds.add(id);
  await commit([{ entity: 'playlist', entityId: id, fields: { $exists: true, name, kind, ownerId: useLibraryStore.getState().userId, isPublic: false, smartRules: opts.smartRules, updatedAt: Date.now() } }]);
  if (opts.tracks?.length) await addToPlaylist(id, opts.tracks);
  return id;
}

export function updatePlaylist(id: string, fields: Partial<Pick<Playlist, 'name' | 'description' | 'isPublic' | 'smartRules' | 'folderId' | 'artworkUrl'>>) {
  return commit([{ entity: 'playlist', entityId: id, fields: { ...fields, updatedAt: Date.now() } }]);
}

export function deletePlaylist(id: string) {
  return commit([{ entity: 'playlist', entityId: id, fields: { $exists: false } }]);
}

/** Converte uma playlist em colaborativa (as próximas ops vão para o escopo compartilhado). */
export async function makeCollaborative(id: string) {
  const p = useLibraryStore.getState().playlists.find((x) => x.id === id);
  if (!p) return;
  collaborativeIds.add(id);
  // Reescreve o estado atual no escopo compartilhado para os colaboradores receberem tudo.
  await commit([
    { entity: 'playlist', entityId: id, fields: { $exists: true, name: p.name, description: p.description, kind: 'collaborative', ownerId: p.ownerId, isPublic: p.isPublic, updatedAt: Date.now() } },
    ...p.entries.map((e) => ({ entity: 'playlistEntry' as const, entityId: `${id}/${e.entryId}`, fields: { $exists: true, trackId: e.trackId, position: e.position, addedBy: e.addedBy, addedAt: e.addedAt } })),
  ]);
}

export async function addToPlaylist(playlistId: string, tracks: Track[]) {
  const p = useLibraryStore.getState().playlists.find((x) => x.id === playlistId);
  const last = p?.entries[p.entries.length - 1]?.position ?? null;
  const keys = keysBetween(last, null, tracks.length);
  const by = useLibraryStore.getState().userId ?? '';
  void cacheTracks(tracks);
  await commit([
    ...tracks.map((t, i) => ({
      entity: 'playlistEntry' as const,
      entityId: `${playlistId}/${Crypto.randomUUID()}`,
      fields: { $exists: true, trackId: t.id, position: keys[i]!, addedBy: by, addedAt: Date.now() },
    })),
    { entity: 'playlist', entityId: playlistId, fields: { updatedAt: Date.now() } },
  ]);
}

export function removeFromPlaylist(playlistId: string, entryId: string) {
  return commit([{ entity: 'playlistEntry', entityId: `${playlistId}/${entryId}`, fields: { $exists: false } }]);
}

/** Arrastar e soltar: gera uma única chave nova entre os vizinhos do destino. */
export function moveEntry(playlistId: string, from: number, to: number) {
  const p = useLibraryStore.getState().playlists.find((x) => x.id === playlistId);
  if (!p || from === to) return;
  const entries = p.entries.filter((_, i) => i !== from);
  const moving = p.entries[from];
  if (!moving) return;
  const before = entries[to - 1]?.position ?? null;
  const after = entries[to]?.position ?? null;
  // Se dois vizinhos têm a mesma chave (edição concorrente), não há espaço: reindexa a lista.
  if (before !== null && after !== null && before >= after) {
    const keys = keysBetween(null, null, p.entries.length);
    const reordered = [...entries.slice(0, to), moving, ...entries.slice(to)];
    return commit(reordered.map((e, i) => ({ entity: 'playlistEntry' as const, entityId: `${playlistId}/${e.entryId}`, fields: { position: keys[i]! } })));
  }
  return commit([{ entity: 'playlistEntry', entityId: `${playlistId}/${moving.entryId}`, fields: { position: keyBetween(before, after) } }]);
}

export function sortPlaylist(playlistId: string, order: string[]) {
  const keys = keysBetween(null, null, order.length);
  return commit(order.map((entryId, i) => ({ entity: 'playlistEntry' as const, entityId: `${playlistId}/${entryId}`, fields: { position: keys[i]! } })));
}
