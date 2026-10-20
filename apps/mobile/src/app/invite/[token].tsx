import { useEffect, useState } from 'react';
import { router, useLocalSearchParams } from 'expo-router';
import { api } from '@/api/client';
import { Empty, Loading } from '@/components/ui';
import { syncNow } from '@/sync/engine';

/** Deep link sonora://invite/<token>: entra como editor de uma playlist colaborativa. */
export default function Invite() {
  const { token } = useLocalSearchParams<{ token: string }>();
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    api
      .acceptInvite(token)
      .then(async ({ playlistId }) => {
        await syncNow();
        router.replace(`/playlist/${playlistId}`);
      })
      .catch((e: Error) => setError(e.message));
  }, [token]);
  return error ? <Empty icon="alert-circle-outline" title="Convite inválido" body={error} /> : <Loading />;
}
