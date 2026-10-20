# Sonora — Arquitetura

Streaming de música **sem anúncios**, com prioridade em **fidelidade de áudio**, **desempenho**, **bateria** e **controle total do usuário**.

## 1. Visão geral

```mermaid
flowchart LR
  subgraph Device["App (Expo / React Native — Android e iOS)"]
    UI[Telas / expo-router] --> Stores[Zustand stores]
    Stores --> Core[@sonora/core<br/>fila · stats · sync · busca · qualidade]
    Stores --> Player[Player engine]
    Player --> Native[expo-audio AudioPlaylist<br/>ExoPlayer / AVQueuePlayer]
    Native --> DSP[SonoraDsp opcional<br/>EQ · bit-perfect]
    Stores --> SQLite[(SQLite WAL<br/>histórico · outbox · cache · downloads)]
    Downloads[Downloads] --> Files[(Arquivos FLAC/ALAC originais)]
  end

  subgraph Cloud["Servidor (Node/Fastify)"]
    API[REST API + JWT] --> Catalog[CatalogService federado]
    Catalog --> P1[Jamendo — CC, FLAC]
    Catalog --> P2[Subsonic/Navidrome — acervo próprio hi-res]
    Catalog --> P3[Provedores licenciados — contrato B2B]
    API --> Sync[Sync log por escopo]
    API --> Stats[Ingestão de plays + rollups]
    API --> Recs[Recomendações]
    API --> Lyrics[LRCLIB]
    Sync & Stats & Recs --> PG[(PostgreSQL + pgvector)]
    Job[Job co-escuta horário] --> PG
  end

  Stores <-- HTTPS --> API
  Native <-- streams HTTPS/CDN --> P1 & P2 & P3
```

Monorepo (npm workspaces):

| Pacote | Conteúdo |
|---|---|
| `packages/core` | Lógica pura e testada, compartilhada por app e servidor: fila (shuffle/repeat/fila do usuário), estatísticas (minutos, streaks, tops, "Wrapped" contínuo), parser LRC, índices fracionários (drag-and-drop), HLC + LWW (sync), parser da busca avançada, seleção de qualidade/ReplayGain, recomendação, playlists inteligentes. |
| `apps/server` | API Fastify, providers de catálogo, repositórios Postgres/memória, recomendações, job de co-escuta, esquema SQL. |
| `apps/mobile` | App Expo SDK 58 / React Native 0.88 (expo-router, expo-audio, expo-sqlite, expo-file-system). |

**Por que React Native/Expo e não Flutter?** O código de domínio (fila, estatísticas, sync, recomendações) é TypeScript puro e roda idêntico no app e no servidor — o número de minutos que o usuário vê no celular é calculado pela mesma função que o servidor usa. O áudio não passa pelo JS: o decodificador e a fila gapless são nativos (ExoPlayer/AVQueuePlayer), então não há penalidade de desempenho na reprodução.

## 2. Catálogo e licenciamento

"O maior catálogo possível" de forma **legal** exige contratos de licenciamento (gravadoras majors, distribuidoras/agregadoras, sociedades de direitos autorais como ECAD no Brasil). Não existe API pública que dê o catálogo do Spotify/Apple Music para terceiros. Por isso o catálogo é **federado e plugável** (`CatalogProvider` em `apps/server/src/providers/types.ts`):

| Provider | Status | Qualidade | Uso |
|---|---|---|---|
| **Jamendo** | Implementado | FLAC 16/44.1 + MP3 | ~600 mil faixas Creative Commons; requer `JAMENDO_CLIENT_ID` grátis. |
| **Subsonic/OpenSubsonic** (Navidrome, Gonic…) | Implementado | Arquivo original (`format=raw`), até 32-bit/384 kHz, ReplayGain | Acervo próprio do usuário/organização. É o caminho para hi-res real hoje. |
| **Provedores licenciados** | Interface pronta | FLAC/ALAC hi-res, Dolby Atmos (E-AC-3 JOC) | Integração via contrato com distribuidoras (feeds DDEX ERN para metadados, CDN própria com DRM). |

Contratos, requisitos de licenciantes e o que já está implementado para eles: ver [LICENSING.md](LICENSING.md).

O `CatalogService`:
- roteia por prefixo do ID (`jamendo:123`, `subsonic:abc`);
- busca em paralelo em todos os providers, tolerando falha de um deles (`errors[]` na resposta);
- aplica os filtros avançados que o provider não suporta (ano, gênero, popularidade, qualidade);
- **deduplica por ISRC** (ou título+artista normalizados) mantendo a versão de maior fidelidade;
- faz cache em memória com TTL (em produção: Redis).

### Busca avançada
Sintaxe aceita na caixa de busca (o app também monta isso pelos filtros visuais):

```
artist:"Milton Nascimento" album:clube year:1970-1979 genre:mpb -genre:funk
popularity:>70 quality:hires type:track lyrics:"travessia"
```

`lyrics:` usa a busca de letras (LRCLIB; em produção, índice full-text `lyrics_cache.tsv` no Postgres) e resolve os resultados no catálogo.

## 3. Backend

### Serviços (hoje um processo; separáveis por rota)

| Rota | Função |
|---|---|
| `POST /auth/register`, `/auth/login`, `GET /me` | Contas (scrypt + JWT 30 d). |
| `GET /search`, `/tracks/:id`, `POST /tracks/batch`, `/albums/:id`, `/artists/:id` | Catálogo federado. |
| `GET /tracks/:id/lyrics` | Letras sincronizadas (LRC). |
| `GET /sync/scopes`, `POST /sync/push`, `GET /sync/pull` | Sincronização multi-dispositivo. |
| `POST /playlists/:id/claim`, `/invites`, `POST /invites/:token/accept`, `/follow` | Playlists colaborativas e seguir playlists. |
| `POST /plays`, `GET /history`, `/stats/summary`, `/stats/series`, `/stats/top` | Histórico e estatísticas (o servidor carimba país e plano em cada reprodução). |
| `GET /tracks/:id/stream`, `POST /downloads/renew` | Autorização por assinatura/território, URL assinada, licença offline de 30 dias. |
| `POST /admin/users/:id/entitlement`, `GET /admin/reports/usage` | Administração (exige `ADMIN_TOKEN`): plano do usuário e relatório mensal de royalties em CSV. |
| `GET /recs/home`, `/recs/discover-weekly`, `/recs/daily-mixes`, `/recs/radio`, `POST /recs/playlist` | Recomendações. |

### Modelo de dados (`apps/server/db/schema.sql`)
- `sync_ops` — log de mudanças (event sourcing) por **escopo**: `user:<id>` (biblioteca pessoal) ou `playlist:<id>` (playlist colaborativa compartilhada).
- `playlist_members`, `playlist_invites` — papéis (`owner`/`editor`/`follower`) e convites com expiração.
- `play_events` — histórico append-only, ID gerado no cliente (reenvio idempotente). Em produção: particionado por mês.
- `listening_daily` — rollup por **dia local** do usuário, atualizado na mesma transação da ingestão (gráficos instantâneos).
- `track_neighbors` — top-K vizinhos de co-escuta por faixa.
- `track_embeddings` — `vector(128)` com índice HNSW (pgvector) para similaridade de conteúdo.
- `lyrics_cache` — letras + `tsvector` gerado para busca por trecho.

Sem `DATABASE_URL`, o servidor usa um repositório em memória com a mesma interface (`Repos`) — é o que os testes usam.

## 4. Recomendações

Pipeline em dois estágios (`packages/core/src/recommend.ts`, `apps/server/src/recs`):

1. **Candidatos**
   - *Co-escuta (item-item)*: o job divide o histórico de todos os usuários em sessões (pausa > 30 min), conta pares numa janela deslizante de 5 faixas com peso `1/distância`, normaliza por cosseno (não favorece só hits) e grava top-50 por faixa.
   - *Conteúdo*: vizinhos por embedding (pgvector) — resolve **cold start** de faixas novas sem histórico. Embeddings de áudio podem vir de um modelo tipo CLAP/MusiCNN rodando em lote na ingestão.
   - *Gênero/artista*: faixas populares dos gêneros dominantes do usuário.
2. **Ranking** — perfil de gosto com **decaimento exponencial** (meia-vida 30 dias); skip conta negativo; faixas muito puladas são enterradas. `discovery` (0..1) equilibra familiar × novo.
3. **Re-ranking por diversidade** — limite por artista e nunca o mesmo artista em sequência; *smart shuffle* espalha artistas uniformemente.

Produtos:
- **Descobertas da Semana**: 30 faixas nunca ouvidas, `discovery=0.9`, 1 por artista, semente = número da semana (estável a semana toda e igual em todos os dispositivos).
- **Daily Mix 1–6**: clusters por gênero dominante das favoritas; ~70% familiar + ~30% novo; semente diária.
- **Rádio de faixa/artista**: infinito — o app pede mais quando faltam ≤ 2 faixas, excluindo as já tocadas.
- **Recomendadas na playlist**: baseadas nas últimas 20 faixas da playlist.

Nada é "conteúdo forçado": mixes aparecem na Home, mas nunca entram na fila sem ação do usuário; o autoplay ao fim da fila pode ser desligado na tela da fila.

## 5. Estatísticas de escuta

**Precisão** — o que conta é tempo **real** ouvido, medido por `ListenClock` (relógio de parede só enquanto o player está em `playing`): pausas, buffering e seeks não inflam o número. Cada troca de faixa fecha um `PlayEvent` com `listenedMs`, fuso horário (`tzOffsetMin`), dispositivo, contexto e se foi skip (< 30 s ou < 50% com troca manual).

**Armazenamento em duas camadas**
- *No aparelho*: SQLite (WAL). Toda a tela de Estatísticas é calculada localmente com `@sonora/core` — instantânea e funciona offline.
- *No servidor*: `play_events` (verdade consolidada de todos os dispositivos) + `listening_daily` para séries rápidas. O cliente envia eventos pendentes em lotes; IDs do cliente tornam o reenvio idempotente.

**Métricas** — minutos/horas por dia/semana ISO/mês/ano (sempre no dia **local** do evento), top músicas/artistas/gêneros por tempo e por plays, streak atual e recorde, taxa de skip, dia de pico, hora favorita, mapa de calor 7×24, artistas novos no período, histórico completo agrupado por dia. O resumo estilo *Wrapped* (`summarize`) aceita **qualquer intervalo, a qualquer momento**.

Escala: com ~50 eventos/usuário/dia, 10 M usuários ≈ 500 M linhas/dia. Caminho de evolução: `play_events` → Kafka → ClickHouse (agregações), mantendo `listening_daily` no Postgres para a UI.

## 6. Sincronização entre dispositivos

- Cada mudança local vira `ChangeOp {entity, entityId, field, value, hlc, deviceId}`.
- **Hybrid Logical Clock** dá ordem total sem depender de relógios sincronizados; resolução **last-writer-wins por campo** → merge comutativo, associativo e idempotente (testado aplicando em ordens diferentes).
- Coleções (curtidas, álbuns salvos, entradas de playlist) usam o campo `$exists` por elemento.
- **Ordem de playlists** usa **índices fracionários**: mover uma faixa gera *uma* chave nova entre os vizinhos. Dois colaboradores reordenando offline não conflitam; empate raro é desfeito por `entryId` e, se não houver espaço entre chaves iguais, a lista é reindexada.
- Fluxo: UI otimista → `outbox` (SQLite) → push por escopo → pull de cada escopo desde o cursor. Dispara ao abrir o app, ao recuperar rede, 1,5 s após mudanças (debounce) e a cada 60 s **somente em primeiro plano** — nenhum polling em segundo plano.
- Progresso de reprodução (fila + faixa) é salvo localmente e pode ser publicado como entidade `playback` para "continuar em outro dispositivo".

Evolução: WebSocket por escopo para playlists colaborativas em tempo real (o protocolo de ops já suporta).

## 7. Desempenho e bateria

- Áudio 100% nativo; o JS só reage a eventos (intervalo de status de 500 ms, não por frame).
- Janela nativa de 3 faixas (atual + 2 pré-carregadas): transição gapless sem acordar o JS.
- Lossless no Wi-Fi, qualidade configurável no celular; downloads no codec original.
- Imagens com `expo-image` (cache memória+disco, reciclagem em listas).
- Tema escuro com preto real (#0A0A0C) — economiza energia em telas OLED.
- Sync sem polling em segundo plano; lotes de 500 ops/eventos.
- SQLite em WAL com `synchronous=NORMAL`.
- Busca com debounce de 300 ms; respostas de catálogo em cache no servidor.

## 8. Segurança e privacidade

- Senhas com scrypt + salt; comparação em tempo constante; JWT no SecureStore (Keychain/Keystore).
- Rate limit global (600 req/min por IP), validação de entrada com zod, limites de tamanho de lote.
- Controle de acesso por escopo no sync (só membros leem; só dono/editor escrevem).
- Sem SDKs de anúncio nem rastreamento publicitário. Dados de escuta são usados apenas para estatísticas e recomendações do próprio usuário (e co-escuta agregada/anônima).

## 9. Deploy sugerido

- API: contêiner stateless (Fly.io/Cloud Run/K8s) atrás de CDN; Postgres gerenciado com pgvector; Redis para cache de catálogo.
- Áudio: servido pelos próprios providers/CDN (o servidor não faz proxy de bytes → custo de banda mínimo).
- App: EAS Build (development build é necessário para background audio e o módulo SonoraDsp), EAS Update para correções JS.
