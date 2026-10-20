import type { ChangeOp, Entitlement, Plan, PlayEvent } from '@sonora/core';

export interface User {
  id: string;
  email: string;
  displayName: string;
  passwordHash: string;
  entitlement: Entitlement;
}

/** Carimbo aplicado pelo servidor na ingestão (o cliente não pode forjar país/plano). */
export interface PlayStamp {
  country: string;
  plan: Plan;
}

export type StampedPlay = PlayEvent & PlayStamp & { userId: string };

export interface StoredOp extends ChangeOp {
  seq: number;
  scope: string;
  authorId: string;
}

export type PlaylistRole = 'owner' | 'editor' | 'follower';

/**
 * Persistência. Implementada em Postgres (produção) e em memória (dev/testes).
 */
export interface Repos {
  users: {
    create(u: Omit<User, 'id'>): Promise<User>;
    byEmail(email: string): Promise<User | null>;
    byId(id: string): Promise<User | null>;
    setEntitlement(id: string, e: Entitlement): Promise<void>;
  };
  sync: {
    /** Grava ops (idempotente) e retorna o maior seq do escopo. */
    append(scope: string, authorId: string, ops: ChangeOp[]): Promise<number>;
    since(scope: string, cursor: number, limit: number): Promise<StoredOp[]>;
  };
  playlists: {
    setMember(playlistId: string, userId: string, role: PlaylistRole): Promise<void>;
    role(playlistId: string, userId: string): Promise<PlaylistRole | null>;
    hasMembers(playlistId: string): Promise<boolean>;
    playlistsOf(userId: string): Promise<Array<{ playlistId: string; role: PlaylistRole }>>;
    createInvite(token: string, playlistId: string, by: string, expiresAt: Date): Promise<void>;
    consumeInvite(token: string): Promise<{ playlistId: string } | null>;
  };
  plays: {
    /** Insere ignorando IDs já existentes. Retorna quantos eram novos. */
    insert(userId: string, events: PlayEvent[], stamp: PlayStamp): Promise<number>;
    list(userId: string, from?: number, to?: number): Promise<PlayEvent[]>;
    /** Eventos de todos os usuários desde `since` (para o job de co-escuta). */
    allSince(since: number, until?: number): Promise<StampedPlay[]>;
  };
  neighbors: {
    replace(trackId: string, neighbors: Array<{ id: string; score: number }>): Promise<void>;
    of(trackId: string, limit: number): Promise<Array<{ id: string; score: number }>>;
  };
}
