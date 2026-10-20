import { AppState } from 'react-native';
import * as Network from 'expo-network';
import { api } from '@/api/client';
import { ackOutbox, getCursor, markPlaysUploaded, outbox, pendingPlays, setCursor } from '@/db/local';
import { applyRemote, useLibrary } from '@/library/store';

/**
 * Sincronização entre dispositivos:
 *  1. push: envia o outbox por escopo (biblioteca pessoal e cada playlist colaborativa);
 *  2. pull: busca ops novas de cada escopo desde o último cursor e aplica (LWW por HLC);
 *  3. plays: envia o histórico de escuta pendente (idempotente por ID).
 * Dispara ao abrir o app, ao voltar a ter rede, após mudanças locais (debounce)
 * e a cada 60 s em primeiro plano. Nada roda em segundo plano → zero custo de bateria parado.
 */

let running = false;
let again = false;
let debounce: ReturnType<typeof setTimeout> | null = null;
let interval: ReturnType<typeof setInterval> | null = null;

export async function syncNow(): Promise<void> {
  if (running) {
    again = true;
    return;
  }
  running = true;
  try {
    const net = await Network.getNetworkStateAsync();
    if (!net.isConnected) return;

    // Playlists colaborativas criadas offline precisam ser registradas antes do primeiro push.
    const known = new Set((await api.scopes()).scopes);
    const pendingScopes = new Set((await outbox(5000)).map((b) => b.scope));
    for (const scope of pendingScopes) {
      if (scope.startsWith('playlist:') && !known.has(scope)) await api.claimPlaylist(scope.slice('playlist:'.length)).catch(() => undefined);
    }

    // 1. push
    for (;;) {
      const batch = await outbox(500);
      if (!batch.length) break;
      const byScope = new Map<string, typeof batch>();
      for (const b of batch) byScope.set(b.scope, [...(byScope.get(b.scope) ?? []), b]);
      for (const [scope, items] of byScope) {
        await api.push(scope, items.map((i) => i.op));
        await ackOutbox(items[items.length - 1]!.seq, scope);
      }
      if (batch.length < 500) break;
    }

    // 2. pull
    const { scopes } = await api.scopes();
    for (const scope of scopes) {
      let cursor = await getCursor(scope);
      for (;;) {
        const r = await api.pull(scope, cursor);
        await applyRemote(r.ops);
        cursor = r.cursor;
        await setCursor(scope, cursor);
        if (!r.hasMore) break;
      }
    }

    // 3. plays
    for (;;) {
      const plays = await pendingPlays(500);
      if (!plays.length) break;
      await api.uploadPlays(plays);
      await markPlaysUploaded(plays.map((p) => p.id));
      if (plays.length < 500) break;
    }
  } catch (e) {
    console.warn('sync falhou; tentará novamente', e);
  } finally {
    running = false;
    if (again) {
      again = false;
      void syncNow();
    }
  }
}

export function startSync() {
  void syncNow();
  useLibrary.subscribe((s, prev) => {
    if (s.dirty !== prev.dirty) {
      if (debounce) clearTimeout(debounce);
      debounce = setTimeout(() => void syncNow(), 1500);
    }
  });
  Network.addNetworkStateListener((s) => {
    if (s.isConnected) void syncNow();
  });
  const startInterval = () => {
    if (!interval) interval = setInterval(() => void syncNow(), 60_000);
  };
  startInterval();
  AppState.addEventListener('change', (st) => {
    if (st === 'active') {
      void syncNow();
      startInterval();
    } else if (interval) {
      clearInterval(interval);
      interval = null;
      void syncNow(); // envia o que falta antes de ir para segundo plano
    }
  });
}
