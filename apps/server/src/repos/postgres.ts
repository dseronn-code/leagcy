import pg from 'pg';
import { localDay, isQualifiedPlay, type Plan, type PlayEvent, type SpotifyItem } from '@sonora/core';
import type { ImportedPlaylist, Repos, StoredOp, User } from './types';

type Row = Record<string, unknown>;

function toPlay(r: Row): PlayEvent {
  return {
    id: r.id as string,
    trackId: r.track_id as string,
    artistIds: r.artist_ids as string[],
    genres: r.genres as string[],
    startedAt: (r.started_at as Date).getTime(),
    listenedMs: r.listened_ms as number,
    trackDurationMs: r.track_duration_ms as number,
    skipped: r.skipped as boolean,
    source: r.source as PlayEvent['source'],
    deviceId: r.device_id as string,
    tzOffsetMin: r.tz_offset_min as number,
  };
}

function toImport(r: Row): ImportedPlaylist {
  return {
    userId: r.user_id as string,
    spotifyId: r.spotify_id as string,
    localId: r.local_id as string,
    name: r.name as string,
    artworkUrl: (r.artwork_url as string | null) ?? undefined,
    snapshotId: r.snapshot_id as string,
    items: r.items as ImportedPlaylist['items'],
    lastSyncedAt: (r.last_synced_at as Date).getTime(),
    lastError: (r.last_error as string | null) ?? null,
  };
}

function toUser(r: Row): User {
  return {
    id: r.id as string,
    email: r.email as string,
    displayName: r.display_name as string,
    passwordHash: r.password_hash as string,
    entitlement: { plan: r.plan as Plan, validUntil: (r.plan_valid_until as Date).getTime(), country: (r.country as string).trim() },
  };
}

export function postgresRepos(connectionString: string): Repos & { close(): Promise<void> } {
  const pool = new pg.Pool({ connectionString, max: 20 });
  const q = async (sql: string, params: unknown[] = []) => (await pool.query(sql, params)).rows as Row[];

  return {
    close: () => pool.end(),
    users: {
      async create(u) {
        const e = u.entitlement;
        const [r] = await q(
          'INSERT INTO users (email, display_name, password_hash, plan, plan_valid_until, country) VALUES ($1,$2,$3,$4,to_timestamp($5/1000.0),$6) RETURNING id',
          [u.email, u.displayName, u.passwordHash, e.plan, e.validUntil, e.country],
        );
        return { ...u, id: r!.id as string };
      },
      async byEmail(email) {
        const [r] = await q('SELECT * FROM users WHERE email = $1', [email]);
        return r ? toUser(r) : null;
      },
      async byId(id) {
        const [r] = await q('SELECT * FROM users WHERE id = $1', [id]);
        return r ? toUser(r) : null;
      },
      async setEntitlement(id, e) {
        await q('UPDATE users SET plan = $2, plan_valid_until = to_timestamp($3/1000.0), country = $4 WHERE id = $1', [id, e.plan, e.validUntil, e.country]);
      },
    },
    sync: {
      async append(scope, authorId, ops) {
        if (ops.length) {
          await q(
            `INSERT INTO sync_ops (scope, entity, entity_id, field, value, hlc, device_id, author_id)
             SELECT $1, o->>'entity', o->>'entityId', o->>'field', o->'value', o->>'hlc', o->>'deviceId', $2
             FROM jsonb_array_elements($3::jsonb) AS o
             ON CONFLICT DO NOTHING`,
            [scope, authorId, JSON.stringify(ops)],
          );
        }
        const [r] = await q('SELECT coalesce(max(seq), 0) AS seq FROM sync_ops WHERE scope = $1', [scope]);
        return Number(r!.seq);
      },
      async since(scope, cursor, limit) {
        const rows = await q('SELECT * FROM sync_ops WHERE scope = $1 AND seq > $2 ORDER BY seq LIMIT $3', [scope, cursor, limit]);
        return rows.map(
          (r): StoredOp => ({
            seq: Number(r.seq),
            scope: r.scope as string,
            entity: r.entity as StoredOp['entity'],
            entityId: r.entity_id as string,
            field: r.field as string,
            value: r.value,
            hlc: r.hlc as string,
            deviceId: r.device_id as string,
            authorId: r.author_id as string,
          }),
        );
      },
    },
    playlists: {
      async setMember(p, u, role) {
        await q('INSERT INTO playlist_members VALUES ($1,$2,$3) ON CONFLICT (playlist_id, user_id) DO UPDATE SET role = $3', [p, u, role]);
      },
      async role(p, u) {
        const [r] = await q('SELECT role FROM playlist_members WHERE playlist_id = $1 AND user_id = $2', [p, u]);
        return (r?.role as 'owner' | 'editor' | 'follower') ?? null;
      },
      async hasMembers(p) {
        return (await q('SELECT 1 FROM playlist_members WHERE playlist_id = $1 LIMIT 1', [p])).length > 0;
      },
      async playlistsOf(u) {
        const rows = await q('SELECT playlist_id, role FROM playlist_members WHERE user_id = $1', [u]);
        return rows.map((r) => ({ playlistId: r.playlist_id as string, role: r.role as 'owner' | 'editor' | 'follower' }));
      },
      async createInvite(token, playlistId, by, expiresAt) {
        await q('INSERT INTO playlist_invites VALUES ($1,$2,$3,$4)', [token, playlistId, by, expiresAt]);
      },
      async consumeInvite(token) {
        const [r] = await q('SELECT playlist_id FROM playlist_invites WHERE token = $1 AND expires_at > now()', [token]);
        return r ? { playlistId: r.playlist_id as string } : null;
      },
    },
    plays: {
      async insert(userId, events, stamp) {
        if (events.length === 0) return 0;
        const client = await pool.connect();
        try {
          await client.query('BEGIN');
          const inserted = await client.query(
            `INSERT INTO play_events
             SELECT e->>'id', $1, ARRAY(SELECT jsonb_array_elements_text(e->'artistIds')), ARRAY(SELECT jsonb_array_elements_text(e->'genres')),
                    to_timestamp((e->>'startedAt')::bigint / 1000.0), (e->>'listenedMs')::int, (e->>'trackDurationMs')::int,
                    (e->>'skipped')::boolean, e->'source', e->>'deviceId', (e->>'tzOffsetMin')::smallint, $3, $4
             FROM jsonb_array_elements($2::jsonb) AS e
             ON CONFLICT (id) DO NOTHING
             RETURNING id`,
            [userId, JSON.stringify(events), stamp.country, stamp.plan],
          );
          // Atualiza o rollup diário apenas com os eventos realmente novos.
          const fresh = new Set(inserted.rows.map((r: Row) => r.id as string));
          const perDay = new Map<string, { ms: number; plays: number }>();
          for (const e of events) {
            if (!fresh.has(e.id)) continue;
            const d = localDay(e.startedAt, e.tzOffsetMin);
            const cur = perDay.get(d) ?? { ms: 0, plays: 0 };
            cur.ms += e.listenedMs;
            cur.plays += isQualifiedPlay(e) ? 1 : 0;
            perDay.set(d, cur);
          }
          for (const [day, v] of perDay) {
            await client.query(
              `INSERT INTO listening_daily VALUES ($1,$2,$3,$4)
               ON CONFLICT (user_id, day) DO UPDATE SET listened_ms = listening_daily.listened_ms + $3, plays = listening_daily.plays + $4`,
              [userId, day, v.ms, v.plays],
            );
          }
          await client.query('COMMIT');
          return fresh.size;
        } catch (err) {
          await client.query('ROLLBACK');
          throw err;
        } finally {
          client.release();
        }
      },
      async list(userId, from = 0, to = 8.64e15) {
        const rows = await q('SELECT * FROM play_events WHERE user_id = $1 AND started_at >= to_timestamp($2/1000.0) AND started_at < to_timestamp($3/1000.0) ORDER BY started_at', [userId, from, to]);
        return rows.map(toPlay);
      },
      async allSince(since, until = 8.64e15) {
        const rows = await q('SELECT * FROM play_events WHERE started_at >= to_timestamp($1/1000.0) AND started_at < to_timestamp($2/1000.0) ORDER BY user_id, started_at', [since, until]);
        return rows.map((r) => ({ ...toPlay(r), userId: r.user_id as string, country: (r.country as string).trim(), plan: r.plan as Plan }));
      },
    },
    neighbors: {
      async replace(trackId, list) {
        await q('DELETE FROM track_neighbors WHERE track_id = $1', [trackId]);
        if (list.length)
          await q(
            `INSERT INTO track_neighbors SELECT $1, n->>'id', (n->>'score')::real FROM jsonb_array_elements($2::jsonb) AS n`,
            [trackId, JSON.stringify(list)],
          );
      },
      async of(trackId, limit) {
        const rows = await q('SELECT neighbor_id, score FROM track_neighbors WHERE track_id = $1 ORDER BY score DESC LIMIT $2', [trackId, limit]);
        return rows.map((r) => ({ id: r.neighbor_id as string, score: r.score as number }));
      },
    },
    spotify: {
      async getLink(userId) {
        const [r] = await q('SELECT * FROM spotify_links WHERE user_id = $1', [userId]);
        return r
          ? { userId, spotifyUserId: r.spotify_user_id as string, displayName: r.display_name as string, refreshToken: r.refresh_token as string, accessToken: r.access_token as string, expiresAt: (r.expires_at as Date).getTime() }
          : null;
      },
      async setLink(l) {
        await q(
          `INSERT INTO spotify_links VALUES ($1,$2,$3,$4,$5,to_timestamp($6/1000.0))
           ON CONFLICT (user_id) DO UPDATE SET spotify_user_id=$2, display_name=$3, refresh_token=$4, access_token=$5, expires_at=to_timestamp($6/1000.0)`,
          [l.userId, l.spotifyUserId, l.displayName, l.refreshToken, l.accessToken, l.expiresAt],
        );
      },
      async deleteLink(userId) {
        await q('DELETE FROM spotify_links WHERE user_id = $1', [userId]);
      },
      async listImports(userId) {
        return (await q('SELECT * FROM spotify_imports WHERE user_id = $1 ORDER BY name', [userId])).map(toImport);
      },
      async allImports() {
        return (await q('SELECT * FROM spotify_imports ORDER BY last_synced_at')).map(toImport);
      },
      async getImport(userId, spotifyId) {
        const [r] = await q('SELECT * FROM spotify_imports WHERE user_id = $1 AND spotify_id = $2', [userId, spotifyId]);
        return r ? toImport(r) : null;
      },
      async saveImport(p) {
        await q(
          `INSERT INTO spotify_imports VALUES ($1,$2,$3,$4,$5,$6,$7::jsonb,to_timestamp($8/1000.0),$9)
           ON CONFLICT (user_id, spotify_id) DO UPDATE SET local_id=$3, name=$4, artwork_url=$5, snapshot_id=$6, items=$7::jsonb, last_synced_at=to_timestamp($8/1000.0), last_error=$9`,
          [p.userId, p.spotifyId, p.localId, p.name, p.artworkUrl ?? null, p.snapshotId, JSON.stringify(p.items), p.lastSyncedAt, p.lastError],
        );
      },
      async deleteImport(userId, spotifyId) {
        await q('DELETE FROM spotify_imports WHERE user_id = $1 AND spotify_id = $2', [userId, spotifyId]);
      },
      async putTracks(items) {
        if (!items.length) return;
        await q(
          `INSERT INTO spotify_tracks SELECT i->>'spotifyId', i FROM jsonb_array_elements($1::jsonb) AS i
           ON CONFLICT (spotify_id) DO UPDATE SET item = EXCLUDED.item`,
          [JSON.stringify(items)],
        );
      },
      async getTrack(id) {
        const [r] = await q('SELECT item FROM spotify_tracks WHERE spotify_id = $1', [id]);
        return r ? (r.item as SpotifyItem) : null;
      },
    },
  };
}
