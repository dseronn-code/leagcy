import { useState } from 'react';
import { Text, TextInput, View, StyleSheet } from 'react-native';
import { router } from 'expo-router';
import { api, setToken } from '@/api/client';
import { Button, Screen } from '@/components/ui';
import { kvSet } from '@/db/local';
import { colors, radius, space, type } from '@/theme';
import { bootstrap } from '@/bootstrap';

export default function Login() {
  const [mode, setMode] = useState<'login' | 'register'>('login');
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async () => {
    setBusy(true);
    setError(null);
    try {
      const r = mode === 'login' ? await api.login(email, password) : await api.register(email, password);
      await setToken(r.token);
      await kvSet('user', r.user);
      await bootstrap();
      router.replace('/');
    } catch (e) {
      setError((e as Error).message);
    } finally {
      setBusy(false);
    }
  };

  return (
    <Screen>
      <View style={{ marginTop: 80, gap: space.lg }}>
        <Text style={type.hero}>Sonora</Text>
        <Text style={type.caption}>Música em qualidade de estúdio. Sem anúncios. Sem interrupções.</Text>
        <TextInput style={styles.input} placeholder="E-mail" placeholderTextColor={colors.textMuted} autoCapitalize="none" keyboardType="email-address" autoComplete="email" value={email} onChangeText={setEmail} />
        <TextInput style={styles.input} placeholder="Senha (mín. 8 caracteres)" placeholderTextColor={colors.textMuted} secureTextEntry autoComplete="password" value={password} onChangeText={setPassword} />
        {error && <Text style={[type.caption, { color: colors.danger }]}>{error}</Text>}
        <Button title={mode === 'login' ? 'Entrar' : 'Criar conta'} onPress={() => void submit()} disabled={busy} />
        <Button variant="secondary" title={mode === 'login' ? 'Não tenho conta' : 'Já tenho conta'} onPress={() => setMode(mode === 'login' ? 'register' : 'login')} />
      </View>
    </Screen>
  );
}

const styles = StyleSheet.create({
  input: { backgroundColor: colors.surface, color: colors.text, borderRadius: radius.md, paddingHorizontal: space.lg, minHeight: 48, fontSize: 16 },
});
