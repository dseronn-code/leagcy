import { useEffect, useState } from 'react';
import { View } from 'react-native';
import { DarkTheme, Stack, ThemeProvider, router } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { SafeAreaProvider } from 'react-native-safe-area-context';
import { TrackSheet } from '@/components/TrackSheet';
import { Loading } from '@/components/ui';
import { bootstrap } from '@/bootstrap';
import { colors } from '@/theme';

const theme = { ...DarkTheme, colors: { ...DarkTheme.colors, background: colors.bg, card: colors.bg, primary: colors.accent, text: colors.text, border: colors.border } };

export default function RootLayout() {
  const [ready, setReady] = useState(false);

  useEffect(() => {
    bootstrap()
      .then((ok) => {
        setReady(true);
        if (!ok) router.replace('/login');
      })
      .catch(() => {
        setReady(true);
        router.replace('/login');
      });
  }, []);

  return (
    <SafeAreaProvider>
      <ThemeProvider value={theme}>
        <StatusBar style="light" />
        {!ready ? (
          <View style={{ flex: 1, backgroundColor: colors.bg, justifyContent: 'center' }}>
            <Loading />
          </View>
        ) : (
          <Stack screenOptions={{ headerShown: false, contentStyle: { backgroundColor: colors.bg }, animation: 'fade_from_bottom' }}>
            <Stack.Screen name="(tabs)" />
            <Stack.Screen name="player" options={{ presentation: 'modal', animation: 'slide_from_bottom' }} />
            <Stack.Screen name="queue" options={{ presentation: 'modal' }} />
            <Stack.Screen name="lyrics" options={{ presentation: 'modal' }} />
            <Stack.Screen name="login" options={{ gestureEnabled: false }} />
          </Stack>
        )}
        <TrackSheet />
      </ThemeProvider>
    </SafeAreaProvider>
  );
}
