# Testes na RTX sem ninguém no PC (runner do GitHub)

O PC com a RTX roda um **runner do GitHub Actions**. Quem desenvolve escreve os comandos em `engine/pedido_rtx.txt` e
envia para o branch de trabalho. O runner:

1. atualiza a pasta do motor (`git fetch` e avanço rápido, sem apagar mudança local);
2. compila (`cmake --build build --config Release`);
3. roda cada linha do pedido na pasta `engine`;
4. publica a saída, o log da compilação e as imagens de `resultados_pc` no branch **`resultados-rtx`**.

O workflow está em `.github/workflows/rtx.yml`. Ele só dispara por push nesse arquivo, no branch de trabalho, e nunca
por pull request.

## Instalação (uma vez, ~10 minutos)

**Antes: deixe o repositório privado.** Um runner no próprio PC num repositório público é um risco, e o próprio
GitHub desaconselha. GitHub → repositório → **Settings → General → Danger Zone → Change visibility → Private**.

1. GitHub → repositório → **Settings → Actions → Runners → New self-hosted runner** → escolha **Windows** e **x64**.
2. A página mostra os comandos de **Download** e **Configure**. Rode-os numa PowerShell, numa pasta nova, por exemplo
   `C:\actions-runner`, sem espaço no caminho.
3. Durante o `config.cmd`, responda:
   - runner group: Enter (padrão);
   - nome do runner: Enter (padrão);
   - **labels adicionais: `rtx`** (importante: o workflow procura esse rótulo);
   - pasta de trabalho: Enter (padrão);
   - rodar como serviço: **N**. Como serviço, o Windows roda sem a sessão do usuário, e a GPU e as permissões da pasta
     do motor podem falhar.
4. Para ligar: abra o **mesmo tipo de janela** que você usa para compilar (a "Developer PowerShell for VS 2022") e rode:

```powershell
cd C:\actions-runner
.\run.cmd
```

Enquanto essa janela estiver aberta e o PC ligado, os testes rodam sozinhos. Para desligar: Ctrl+C ou fechar a janela.

## Cuidados

- O runner usa a pasta `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\leagcy\engine` (a mesma
  de sempre). Não edite arquivos ali enquanto ele roda.
- O PC não pode estar em suspensão. Ajuste em Configurações → Sistema → Energia.
- Se você estiver jogando ou usando a GPU, os números de desempenho saem errados.
