import { api, loadToken } from '@/api/client';
import { kvGet, kvSet, openDb } from '@/db/local';
import { initDownloads } from '@/downloads/manager';
import { initLibrary } from '@/library/store';
import { loadEq } from '@/player/dsp';
import { initPlayer } from '@/player/engine';
import { startSync } from '@/sync/engine';

let done = false;

/** Inicializa tudo uma vez, após o login. Ordem importa: DB → biblioteca → downloads → player. */
export async function bootstrap(): Promise<boolean> {
  if (done) return true;
  await openDb();
  const token = await loadToken();
  if (!token) return false;
  let user = await kvGet<{ id: string }>('user');
  try {
    user = await api.me();
    await kvSet('user', user);
  } catch {
    // offline: segue com o usuário em cache
  }
  if (!user) return false;
  await initLibrary(user.id);
  await initDownloads();
  await loadEq();
  await initPlayer();
  startSync();
  done = true;
  return true;
}

