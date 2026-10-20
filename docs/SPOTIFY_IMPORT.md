# Importar playlists do Spotify

Você cola o link de uma playlist do Spotify no Sonora e ela vira uma playlist na sua biblioteca, que **acompanha o Spotify sozinha**: música adicionada lá aparece aqui, música removida lá some daqui.

## De onde vem o som

O Spotify só permite que outros apps **leiam a lista de músicas**. Baixar ou retransmitir o áudio deles (ou "puxar" do YouTube) viola os termos de uso e o direito autoral, e o Sonora não faz isso. Cada música da playlist é procurada nas **suas fontes legais**:

1. **Seu acervo** num servidor Navidrome/Subsonic (`SUBSONIC_URL`): os arquivos que você tem (comprados em lojas como Bandcamp ou Qobuz, CDs digitalizados...) tocam no **arquivo original, em lossless/hi-res**.
2. **Jamendo** (Creative Commons), se configurado.

Músicas que não forem encontradas continuam visíveis na playlist (esmaecidas) com a opção **Abrir no Spotify**, e ficam fora da fila. O servidor procura de novo essas faixas uma vez por dia, então se você colocar o arquivo no seu acervo, ela passa a tocar sozinha.

O casamento usa o código ISRC quando existe; senão compara título (ignorando "Remastered", "feat." etc.), artista e duração. Em caso de dúvida a faixa fica como "não encontrada": é melhor do que tocar a música errada.

## Configuração (uma vez)

1. Entre em [developer.spotify.com/dashboard](https://developer.spotify.com/dashboard) e crie um app.
2. Em **Redirect URIs**, cadastre exatamente: `sonora://spotify-callback`.
3. Marque o uso de **Web API**.
4. Copie o **Client ID** para `apps/server/.env`: `SPOTIFY_CLIENT_ID=...`. Não é preciso client secret: o login usa PKCE.
5. Em **User Management** do app, adicione o e-mail da conta Spotify que vai conectar.

Regras atuais do Spotify para apps em *Development Mode* (desde fevereiro/2026): o dono do app precisa ter **Spotify Premium** e o app atende **até 5 contas**. Para uso pessoal isso basta.

## Como usar

Biblioteca → **Importar do Spotify** → **Conectar com o Spotify** (só na primeira vez) → cole o link (`https://open.spotify.com/playlist/...`, também aceita `spotify:playlist:...`) → **Importar**.

O Spotify só libera a leitura de playlists que **você criou ou em que colabora**. Para uma playlist de outra pessoa, use *Adicionar a outra playlist* no Spotify e importe a sua cópia.

## Como a sincronização funciona

- O Spotify não avisa quando uma playlist muda (não há webhooks). O servidor consulta o `snapshot_id` de cada playlist importada **a cada 5 minutos** (`SPOTIFY_SYNC_INTERVAL_MS`), o que custa uma chamada leve por playlist. O app também pede uma conferência quando é aberto.
- Só quando o `snapshot_id` muda o servidor baixa as faixas, calcula o que entrou, saiu ou mudou de ordem, e grava isso como mudanças na sua biblioteca. O app recebe pelo mesmo sync de sempre, então não precisa falar com o Spotify e funciona em qualquer aparelho conectado na sua conta.
- A sincronização é de mão única (Spotify → Sonora). Para mudar a playlist, edite no Spotify. **Parar de sincronizar** mantém a playlist no Sonora como playlist comum e editável.
- Faixas repetidas, reordenação e episódios de podcast (ignorados) são tratados.

## Endpoints

| Rota | Função |
|---|---|
| `GET /spotify/status` | Integração ligada? Conta conectada? |
| `POST /spotify/connect` / `DELETE /spotify/connect` | Vincular (código OAuth + PKCE) / desvincular a conta. |
| `GET /spotify/playlists` | Playlists importadas: total, tocáveis, última sincronização, erro. |
| `POST /spotify/playlists` `{url}` | Importa pelo link. |
| `POST /spotify/sync` | Confere todas as playlists do usuário agora (no máximo 1 vez a cada 20 s). |
| `POST /spotify/playlists/:id/sync`, `DELETE /spotify/playlists/:id` | Sincronizar uma / parar de sincronizar. |
