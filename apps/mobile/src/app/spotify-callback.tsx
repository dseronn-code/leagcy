import { useEffect } from 'react';
import { router } from 'expo-router';
import * as WebBrowser from 'expo-web-browser';
import { Loading } from '@/components/ui';

/**
 * Destino do redirect do OAuth (sonora://spotify-callback). O expo-auth-session
 * já capturou o código; esta rota só evita a tela de "rota não encontrada".
 */
export default function SpotifyCallback() {
  useEffect(() => {
    WebBrowser.maybeCompleteAuthSession();
    if (router.canGoBack()) router.back();
    else router.replace('/spotify');
  }, []);
  return <Loading />;
}
