# Sonora

Streaming de música para **Android e iOS**, **sem anúncios**, com áudio **lossless e hi-res** (FLAC/ALAC até 24-bit/192 kHz e além), e as principais funções do Spotify: fila completa, playlists (normais, colaborativas, inteligentes e algorítmicas), biblioteca, páginas de artista e álbum, letras sincronizadas, modo offline, sincronização entre dispositivos e estatísticas de escuta precisas e contínuas.

> "Sonora" é um nome provisório. Para trocar, edite `name`/`slug`/`scheme` em `apps/mobile/app.json`.

## Documentação

- [Arquitetura](docs/ARCHITECTURE.md): catálogo e licenciamento, backend, recomendações, estatísticas, sincronização, desempenho e segurança.
- [Motor de áudio](docs/AUDIO_ENGINE.md): qualidade, gapless, normalização, EQ, bit-perfect, Bluetooth/AirPlay/Chromecast/carros.
- [Telas e fluxos](docs/SCREENS_AND_FLOWS.md): mapa de navegação, telas, fluxos do usuário e acessibilidade.
- [Plano de licenciamento](docs/LICENSING.md): direitos envolvidos, caminhos (parceiro B2B, contratos diretos, nicho), o que o código já cumpre e o que falta.

## Estrutura

```
packages/core   Lógica pura compartilhada (TypeScript, testada): fila, estatísticas, LRC,
                índices fracionários, sync HLC/LWW, busca avançada, qualidade, recomendação
apps/server     API Fastify + PostgreSQL (ou memória em dev), catálogo federado, recomendações
apps/mobile     App Expo SDK 58 / React Native 0.88 (expo-router, expo-audio, expo-sqlite)
docs/           Arquitetura, motor de áudio, telas e fluxos
```

## Rodando

Requisitos: Node 20+; para o app, um development build do Expo (o Expo Go não tem áudio em segundo plano completo).

```bash
npm install

# Testes e typecheck
npm test
npm run typecheck

# Servidor (sem DATABASE_URL usa memória)
cp apps/server/.env.example apps/server/.env   # preencha JAMENDO_CLIENT_ID e/ou SUBSONIC_*
npm run server:dev

# Postgres (opcional)
psql "$DATABASE_URL" -f apps/server/db/schema.sql

# App
# ajuste "extra.apiUrl" em apps/mobile/app.json para o IP do servidor
cd apps/mobile
npx expo run:android   # ou run:ios, ou: npx eas-cli@latest build --profile development
```

### Fontes de música (legais)
- **Jamendo**: catálogo Creative Commons com FLAC. Crie um `client_id` grátis em devportal.jamendo.com.
- **Subsonic/Navidrome**: aponte `SUBSONIC_URL` para o seu servidor e o app toca o acervo **no arquivo original** (hi-res, sem transcodificação).
- **Catálogo comercial** (majors e distribuidoras): exige contratos de licenciamento — ver o [plano de licenciamento](docs/LICENSING.md). O servidor já faz autorização por assinatura e território, URLs assinadas, licença offline de 30 dias e relatório mensal de royalties por ISRC.

## Status

| Área | Status |
|---|---|
| Fila (fila do usuário, shuffle, repeat off/all/one, reordenar, pular para item), seek, velocidade, timer de sono | ✅ |
| Reprodução gapless nativa, lock screen e controles Bluetooth | ✅ |
| Seleção lossless/hi-res/Atmos por rede, selo de qualidade real, normalização ReplayGain sem clipping | ✅ |
| Busca avançada (artista, álbum, gênero, ano, popularidade, qualidade, letra) | ✅ |
| Playlists: CRUD, arrastar e soltar, ordenar, compartilhar, colaborativas com convite, inteligentes por regras | ✅ |
| Playlists algorítmicas: Descobertas da Semana, Daily Mixes, rádio infinito, recomendadas na playlist | ✅ |
| Biblioteca: curtidas, álbuns salvos, artistas e playlists seguidos; páginas de artista/álbum | ✅ |
| Letras sincronizadas (LRCLIB) com seek por linha | ✅ |
| Estatísticas: minutos/horas por dia/semana/mês/ano, tops, gêneros, streaks, mapa de calor, Wrapped contínuo, histórico completo | ✅ |
| Offline: downloads seletivos no codec original, estatísticas offline | ✅ |
| Sincronização entre dispositivos (biblioteca, playlists, configurações, histórico) | ✅ |
| Requisitos de licenciamento: assinatura, território, URLs assinadas, licença offline 30 dias, relatório de royalties (CSV por ISRC/território/plano) | ✅ |
| DRM (Widevine/FairPlay), cobrança via App Store/Google Play, formato DDEX DSR | 🛠️ depende do contrato (ver [licenciamento §5](docs/LICENSING.md#5-o-que-ainda-falta-depende-do-contrato)) |
| Equalizador 10 bandas: UI, presets, persistência e ponte JS | ✅; processamento no módulo nativo `SonoraDsp`, que ainda falta (esboço em docs) |
| Bit-perfect USB (Android 14+) | 🛠️ toggle e ponte prontos; depende do `SonoraDsp` |
| AirPlay | ✅ pelo seletor do sistema; 🛠️ botão dedicado |
| Chromecast, Android Auto, CarPlay | 🛠️ planejado (ver [Motor de áudio §7](docs/AUDIO_ENGINE.md#7-saídas-externas)) |
| "Continuar em outro dispositivo" (handoff de reprodução em tempo real) | 🛠️ planejado (entidade `playback` já existe no protocolo de sync) |

## Verificação

- `packages/core`: 26 testes (fila, estatísticas, LRC, índices fracionários, convergência do sync, busca, qualidade, smart playlists, recomendação, autorização e rateio de royalties).
- `apps/server`: 9 testes de API ponta a ponta com providers falsos (auth, busca filtrada, ingestão idempotente e estatísticas, sync e permissões de playlist colaborativa, recomendações, autorização de stream por assinatura/território, renovação de licença offline, relatório de uso em CSV).
- `apps/mobile`: `tsc --noEmit` e `eslint` (eslint-config-expo) sem erros; bundles Android e iOS gerados com `expo export`. A interface ainda não foi testada num aparelho ou emulador.
