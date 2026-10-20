import * as SQLite from 'expo-sqlite';
import type { ChangeOp, PlayEvent, Track } from '@sonora/core';

/**
 * Banco local (SQLite, modo WAL). Fonte da verdade offline para:
 *  - histórico de reprodução e estatísticas (funciona sem rede);
 *  - outbox de mudanças a sincronizar;
 *  - cache de metadados de faixas e índice de downloads.
 */
let db: SQLite.SQLiteDatabase | null = null;

export async function openDb(): Promise<SQLite.SQLiteDatabase> {
  if (db) return db;
  db = await SQLite.openDatabaseAsync('sonora.db');
  await db.execAsync(`
    PRAGMA journal_mode = WAL;
    PRAGMA synchronous = NORMAL;
    CREATE TABLE IF NOT EXISTS play_events (
      id TEXT PRIMARY KEY,
      json TEXT NOT NULL,
      started_at INTEGER NOT NULL,
      uploaded INTEGER NOT NULL DEFAULT 0
    );
    CREATE INDEX IF NOT EXISTS play_events_time ON play_events (started_at);
    CREATE TABLE IF NOT EXISTS outbox (
      seq INTEGER PRIMARY KEY AUTOINCREMENT,
      scope TEXT NOT NULL,
      json TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS ops (
      key TEXT PRIMARY KEY,           -- entity/entityId|field
      json TEXT NOT NULL
    );
    CREATE TABLE IF NOT EXISTS cursors (scope TEXT PRIMARY KEY, cursor INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS tracks (id TEXT PRIMARY KEY, json TEXT NOT NULL, cached_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS downloads (
      track_id TEXT PRIMARY KEY,
      uri TEXT NOT NULL,
      codec TEXT NOT NULL,
      bytes INTEGER NOT NULL,
      created_at INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT NOT NULL);
  `);
  return db;
}

async function d() {
  return db ?? openDb();
}

// ---------- Histórico ----------
export async function insertPlay(e: PlayEvent) {
  await (await d()).runAsync('INSERT OR IGNORE INTO play_events (id, json, started_at) VALUES (?, ?, ?)', e.id, JSON.stringify(e), e.startedAt);
}

export async function playsBetween(from: number, to: number): Promise<PlayEvent[]> {
  const rows = await (await d()).getAllAsync<{ json: string }>('SELECT json FROM play_events WHERE started_at >= ? AND started_at < ? ORDER BY started_at', from, to);
  return rows.map((r) => JSON.parse(r.json) as PlayEvent);
}

export async function pendingPlays(limit = 500): Promise<PlayEvent[]> {
  const rows = await (await d()).getAllAsync<{ json: string }>('SELECT json FROM play_events WHERE uploaded = 0 ORDER BY started_at LIMIT ?', limit);
  return rows.map((r) => JSON.parse(r.json) as PlayEvent);
}

export async function markPlaysUploaded(ids: string[]) {
  if (!ids.length) return;
  await (await d()).runAsync(`UPDATE play_events SET uploaded = 1 WHERE id IN (${ids.map(() => '?').join(',')})`, ...ids);
}

// ---------- Sync ----------
export async function enqueueOps(scope: string, ops: ChangeOp[]) {
  const db = await d();
  await db.withTransactionAsync(async () => {
    for (const op of ops) await db.runAsync('INSERT INTO outbox (scope, json) VALUES (?, ?)', scope, JSON.stringify(op));
  });
}

export async function outbox(limit = 500): Promise<{ seq: number; scope: string; op: ChangeOp }[]> {
  const rows = await (await d()).getAllAsync<{ seq: number; scope: string; json: string }>('SELECT * FROM outbox ORDER BY seq LIMIT ?', limit);
  return rows.map((r) => ({ seq: r.seq, scope: r.scope, op: JSON.parse(r.json) as ChangeOp }));
}

export async function ackOutbox(maxSeq: number, scope: string) {
  await (await d()).runAsync('DELETE FROM outbox WHERE seq <= ? AND scope = ?', maxSeq, scope);
}

export async function saveOps(ops: ChangeOp[]) {
  const db = await d();
  await db.withTransactionAsync(async () => {
    for (const op of ops) await db.runAsync('INSERT OR REPLACE INTO ops (key, json) VALUES (?, ?)', `${op.entity}/${op.entityId}|${op.field}`, JSON.stringify(op));
  });
}

export async function loadOps(): Promise<ChangeOp[]> {
  const rows = await (await d()).getAllAsync<{ json: string }>('SELECT json FROM ops');
  return rows.map((r) => JSON.parse(r.json) as ChangeOp);
}

export async function getCursor(scope: string): Promise<number> {
  const r = await (await d()).getFirstAsync<{ cursor: number }>('SELECT cursor FROM cursors WHERE scope = ?', scope);
  return r?.cursor ?? 0;
}

export async function setCursor(scope: string, cursor: number) {
  await (await d()).runAsync('INSERT OR REPLACE INTO cursors VALUES (?, ?)', scope, cursor);
}

// ---------- Cache de faixas ----------
export async function cacheTracks(tracks: Track[]) {
  const db = await d();
  const now = Date.now();
  await db.withTransactionAsync(async () => {
    for (const t of tracks) await db.runAsync('INSERT OR REPLACE INTO tracks VALUES (?, ?, ?)', t.id, JSON.stringify(t), now);
  });
}

export async function cachedTracks(ids: string[]): Promise<Map<string, Track>> {
  if (!ids.length) return new Map();
  const rows = await (await d()).getAllAsync<{ json: string }>(`SELECT json FROM tracks WHERE id IN (${ids.map(() => '?').join(',')})`, ...ids);
  return new Map(rows.map((r) => {
    const t = JSON.parse(r.json) as Track;
    return [t.id, t];
  }));
}

// ---------- Downloads ----------
export interface DownloadRow {
  track_id: string;
  uri: string;
  codec: string;
  bytes: number;
  created_at: number;
}

export async function addDownload(r: DownloadRow) {
  await (await d()).runAsync('INSERT OR REPLACE INTO downloads VALUES (?, ?, ?, ?, ?)', r.track_id, r.uri, r.codec, r.bytes, r.created_at);
}

export async function removeDownload(trackId: string) {
  await (await d()).runAsync('DELETE FROM downloads WHERE track_id = ?', trackId);
}

export async function listDownloads(): Promise<DownloadRow[]> {
  return (await d()).getAllAsync<DownloadRow>('SELECT * FROM downloads ORDER BY created_at DESC');
}

// ---------- KV ----------
export async function kvGet<T>(k: string): Promise<T | null> {
  const r = await (await d()).getFirstAsync<{ v: string }>('SELECT v FROM kv WHERE k = ?', k);
  return r ? (JSON.parse(r.v) as T) : null;
}

export async function kvSet(k: string, v: unknown) {
  await (await d()).runAsync('INSERT OR REPLACE INTO kv VALUES (?, ?)', k, JSON.stringify(v));
}
