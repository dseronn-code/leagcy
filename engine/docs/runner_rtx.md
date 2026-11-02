# Testes na RTX sem ninguém no PC (agente)

O PC com a RTX roda `engine/tools/agente_rtx.ps1`. A cada 30 segundos, ele busca o branch de trabalho no GitHub.
Quando `engine/pedido_rtx.txt` muda, ele:

1. atualiza a pasta do motor (só avanço rápido, sem apagar mudança local);
2. compila (`cmake --build build --config Release`);
3. roda cada linha do pedido na pasta `engine`;
4. publica a saída, o log da compilação e as imagens de `resultados_pc` no branch **`resultados-rtx`**.

O agente usa o git do próprio PC, com as credenciais de sempre. Por isso funciona com o repositório privado e não
depende do GitHub Actions. O Actions foi a primeira tentativa (runner self-hosted), mas a cobrança da conta bloqueia
o Actions em repositório privado.

## Ligar

Abra a "Developer PowerShell for VS 2022" e rode, um comando por vez:

```powershell
cd "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\leagcy\engine"
git pull
powershell -ExecutionPolicy Bypass -File tools\agente_rtx.ps1
```

Para parar: Ctrl+C ou fechar a janela. Ao ligar de novo, ele só roda pedidos novos. O último pedido rodado fica
guardado em `%LOCALAPPDATA%\legacy_agente_rtx.txt`.

## Cuidados

- O PC não pode estar em suspensão. Ajuste em Configurações → Sistema → Energia.
- Se você estiver jogando ou usando a GPU, os números de desempenho saem errados.
- O runner do GitHub Actions (`C:\actions-runner`) não é mais usado. Ele pode ser removido em Settings → Actions →
  Runners.
