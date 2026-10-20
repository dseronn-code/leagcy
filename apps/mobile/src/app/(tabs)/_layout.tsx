import { View, type ColorValue } from 'react-native';
import { BottomTabBar, Tabs } from 'expo-router/js-tabs';
import { Ionicons } from '@expo/vector-icons';
import { MiniPlayer } from '@/components/MiniPlayer';
import { colors } from '@/theme';

type Icon = keyof typeof Ionicons.glyphMap;
const icon = (name: Icon, active: Icon) =>
  function TabIcon({ focused, color }: { focused: boolean; color: ColorValue | null | undefined }) {
    return <Ionicons name={focused ? active : name} size={24} color={typeof color === 'string' ? color : colors.text} />;
  };

export default function TabsLayout() {
  return (
    <Tabs
      screenOptions={{
        headerShown: false,
        tabBarActiveTintColor: colors.text,
        tabBarInactiveTintColor: colors.textMuted,
        tabBarStyle: { backgroundColor: colors.bg, borderTopColor: colors.border },
      }}
      tabBar={(props) => (
        <View style={{ backgroundColor: colors.bg }}>
          <MiniPlayer />
          <BottomTabBar {...props} />
        </View>
      )}
    >
      <Tabs.Screen name="index" options={{ title: 'Início', tabBarIcon: icon('home-outline', 'home') }} />
      <Tabs.Screen name="search" options={{ title: 'Buscar', tabBarIcon: icon('search-outline', 'search') }} />
      <Tabs.Screen name="library" options={{ title: 'Biblioteca', tabBarIcon: icon('library-outline', 'library') }} />
      <Tabs.Screen name="stats" options={{ title: 'Estatísticas', tabBarIcon: icon('stats-chart-outline', 'stats-chart') }} />
    </Tabs>
  );
}
