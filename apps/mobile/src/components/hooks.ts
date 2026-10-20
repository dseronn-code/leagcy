import { useCallback, useEffect, useState } from 'react';

/**
 * Carrega dados assíncronos com estado de loading/erro e recarga manual.
 * `deps` devem ser valores serializáveis (ids, flags).
 */
export function useAsync<T>(fn: () => Promise<T>, deps: unknown[]): { data: T | null; error: Error | null; loading: boolean; reload: () => void } {
  const [n, setN] = useState(0);
  const token = JSON.stringify([...deps, n]);
  const [state, setState] = useState<{ token: string | null; data: T | null; error: Error | null }>({ token: null, data: null, error: null });

  useEffect(() => {
    let alive = true;
    fn()
      .then((data) => alive && setState({ token, data, error: null }))
      .catch((error: Error) => alive && setState((s) => ({ token, data: s.data, error })));
    return () => {
      alive = false;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [token]);

  return { data: state.data, error: state.error, loading: state.token !== token, reload: useCallback(() => setN((x) => x + 1), []) };
}

export function useDebounced<T>(value: T, ms: number): T {
  const [v, setV] = useState(value);
  useEffect(() => {
    const t = setTimeout(() => setV(value), ms);
    return () => clearTimeout(t);
  }, [value, ms]);
  return v;
}
