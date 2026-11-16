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

## Desligar o PC pelo celular

Com o agente ligado, abra no navegador do celular (logado no GitHub):

https://github.com/seronzera/leagcy/edit/ccr-5423dd89-y3br6e/engine/comando_pc.txt

Troque a última linha por **`desligar`** (ou `reiniciar`, ou `cancelar`) e toque em **Commit changes**. Em até 30
segundos, o Windows mostra o aviso e desliga em 60 segundos, com um desligamento normal. Cada vez que você salva, o
comando vale uma vez. Na primeira vez que o agente liga, ele só anota o arquivo e não executa nada.

Ligar de longe não é possível por programa, porque com o PC desligado nada roda nele. Para isso, use uma tomada
inteligente Wi-Fi com a opção da BIOS "Restore on AC Power Loss" = Power On, ou Wake-on-LAN com um aparelho sempre
ligado na rede de casa.

## Sinal de vida

A cada 10 minutos, e a cada teste, o agente publica no branch `status-rtx` (arquivo `status.txt`) a hora e o que
está fazendo. Assim, de longe, dá para saber se o PC está ligado. Cada comando do pedido tem limite de 20 minutos:
se passar disso, o agente encerra o comando e segue.

## Cuidados

- O PC não pode estar em suspensão. Ajuste em Configurações → Sistema → Energia.
- Se você estiver jogando ou usando a GPU, os números de desempenho saem errados.
- O runner do GitHub Actions (`C:\actions-runner`) não é mais usado. Ele pode ser removido em Settings → Actions →
  Runners.
