import { randomUUID } from 'node:crypto';
import type { ChangeOp, PlayEvent } from '@sonora/core';
import type { PlaylistRole, Repos, StoredOp, User } from './types';

/** Implementação em memória — usada em testes e quando não há DATABASE_URL. */
export function memoryRepos(): Repos {
  const users = new Map<string, User>();
  const ops: StoredOp[] = [];
  const opKeys = new Set<string>();
  const members = new Map<string, PlaylistRole>();
  const invites = new Map<string, { playlistId: string; expiresAt: Date }>();
  const plays = new Map<string, PlayEvent & { userId: string }>();
  const neighbors = new Map<string, Array<{ id: string; score: number }>>();
  let seq = 0;

  return {
    users: {
      async create(u) {
        const user = { ...u, id: randomUUID() };
        users.set(user.id, user);
        return user;
      },
      async byEmail(email) {
        return [...users.values()].find((u) => u.email === email) ?? null;
      },
      async byId(id) {
        return users.get(id) ?? null;
      },
    },
    sync: {
      async append(scope, authorId, newOps: ChangeOp[]) {
        for (const op of newOps) {
          const k = `${scope}|${op.entity}|${op.entityId}|${op.field}|${op.hlc}`;
          if (opKeys.has(k)) continue;
          opKeys.add(k);
          ops.push({ ...op, seq: ++seq, scope, authorId });
        }
        return ops.filter((o) => o.scope === scope).reduce((m, o) => Math.max(m, o.seq), 0);
      },
      async since(scope, cursor, limit) {
        return ops.filter((o) => o.scope === scope && o.seq > cursor).slice(0, limit);
      },
    },
    playlists: {
      async setMember(p, u, role) {
        members.set(`${p}|${u}`, role);
      },
      async role(p, u) {
        return members.get(`${p}|${u}`) ?? null;
      },
      async hasMembers(p) {
        return [...members.keys()].some((k) => k.startsWith(`${p}|`));
      },
      async playlistsOf(u) {
        return [...members.entries()]
          .filter(([k]) => k.endsWith(`|${u}`))
          .map(([k, role]) => ({ playlistId: k.slice(0, k.lastIndexOf('|')), role }));
      },
      async createInvite(token, playlistId, _by, expiresAt) {
        invites.set(token, { playlistId, expiresAt });
      },
      async consumeInvite(token) {
        const inv = invites.get(token);
        if (!inv || inv.expiresAt.getTime() < Date.now()) return null;
        return { playlistId: inv.playlistId };
      },
    },
    plays: {
      async insert(userId, events) {
        let n = 0;
        for (const e of events) {
          if (plays.has(e.id)) continue;
          plays.set(e.id, { ...e, userId });
          n++;
        }
        return n;
      },
      async list(userId, from = 0, to = Number.MAX_SAFE_INTEGER) {
        return [...plays.values()]
          .filter((e) => e.userId === userId && e.startedAt >= from && e.startedAt < to)
          .sort((a, b) => a.startedAt - b.startedAt)
          .map(({ userId: _u, ...e }) => e);
      },
      async allSince(since) {
        return [...plays.values()].filter((e) => e.startedAt >= since);
      },
    },
    neighbors: {
      async replace(trackId, list) {
        neighbors.set(trackId, list);
      },
      async of(trackId, limit) {
        return (neighbors.get(trackId) ?? []).slice(0, limit);
      },
    },
  };
}
