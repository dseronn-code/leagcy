# O que tem no motor do GTA 6 e o que falta no Legacy Engine

Pesquisa de 22/10/2026, status atualizado no Dia 2. O GTA 6 está previsto para 19/11/2026 (PS5 e Xbox Series).

**Sobre as fontes:** a Rockstar quase não publica detalhes técnicos do RAGE. O nome "RAGE 9" aparece só em blogs de fãs. O que existe de mais confiável é a análise da Digital Foundry sobre o Trailer 2 (capturado em um PS5 comum). O resto da lista vem do que já se sabe do GTA V e do RDR2, de patentes da Take-Two e do que todo mundo aberto AAA precisa.

## O que se sabe do GTA 6

- **Iluminação global 100% por ray tracing (RTGI)**, calculada em tempo real. O RDR2 usava luz pré-calculada.
- **Reflexos híbridos:** ray tracing em rios, vidros, janelas de carro e óculos; screen-space (SSR) em superfícies grandes de água.
- **Sombras do sol** reportadas como shadow maps com penumbra variável (não ray tracing).
- **Cabelo por fios (strands)**: caro, com otimização dinâmica.
- **Detalhe de personagens:** suor, pele e roupa animados separadamente do corpo.
- **Resolução:** cerca de 1152p–1440p interno, reconstruído para 4K, a 30 FPS. A DF acha improvável um modo 60 FPS no lançamento.
- **Patentes:** locomoção montada a partir de pequenos blocos de animação (reagindo a clima, cansaço, ferimentos) e IA de NPCs e veículos em redes de navegação.

## Comparação

✅ temos · 🟡 parcial · ❌ falta

### Iluminação

| Recurso | Legacy | Observação |
|---|---|---|
| GI por ray tracing | 🟡 | 1 rebote com denoiser SVGF (Dia 2); falta cache de radiância multi-rebote |
| Reflexos por ray tracing | ✅ | VNDF + denoiser (Dia 2); SSR híbrido não necessário por enquanto |
| Sombra do sol | ✅ | Ray tracing, com penumbra física |
| Shadow maps em cascata (fallback e performance) | ❌ | |
| Milhares de luzes (neon, faróis, postes) | ✅ | ReSTIR DI com sombras (Dia 2) |
| Céu e atmosfera física | ✅ | |
| Nuvens volumétricas, raios de luz (god rays) | ❌ | Hoje só temos névoa por altura |
| Ciclo dia/noite | 🟡 | O sol se move; faltam lua, estrelas e luzes da cidade |

### Imagem

| Recurso | Legacy | Observação |
|---|---|---|
| Denoiser | ✅ | SVGF (Dia 2); evolução: ReLAX/ReBLUR (NVIDIA NRD) |
| TAA | ✅ | TAAU com clipping YCoCg (Dia 2) |
| Upscaling (FSR, DLSS, XeSS) | 🟡 | TAAU próprio (Dia 2); faltam DLSS/FSR 3/XeSS |
| Pós-processamento: bloom, profundidade de campo, motion blur, auto-exposição, lens flare | 🟡 | Tudo menos lens flare (Dia 2) |

### Materiais e superfícies

| Recurso | Legacy | Observação |
|---|---|---|
| PBR | ✅ | Com texturas glTF (Dia 2) |
| Texturas, normal maps, mipmaps, compressão BC7 | 🟡 | Tudo menos BC7 (Dia 2) |
| Pintura de carro (clearcoat, flocos metálicos) | ❌ | |
| Vidro e transparência | ❌ | |
| Decals (sujeira, marcas, pichação) | ❌ | |
| Chuva, superfícies molhadas e poças | ❌ | |
| Água: oceano FFT, ondas, rios, espuma, cena subaquática | ❌ | Miami ficcional: água em todo lugar |

### Personagens

| Recurso | Legacy | Observação |
|---|---|---|
| Skinning (esqueleto) | ❌ | Pré-requisito de qualquer personagem |
| Pele com subsurface scattering, olhos | ❌ | |
| Cabelo por fios | ❌ | |
| Tecido simulado | ❌ | |
| Multidões (centenas de NPCs animados) | ❌ | Instancing + LOD de animação + impostors |

### Mundo aberto

| Recurso | Legacy | Observação |
|---|---|---|
| Streaming do mundo | ❌ | O item mais difícil: carregar e descarregar o mapa enquanto o jogador anda |
| LOD e HLOD de cidade inteira | ❌ | |
| Renderização GPU-driven (culling em compute, oclusão, meshlets) | ❌ | Sem isso não dá para desenhar milhões de objetos |
| Vegetação instanciada com vento | ❌ | |
| Terreno grande (clipmaps) | 🟡 | Temos terreno procedural de tamanho fixo |
| Partículas e VFX (fogo, fumaça, explosões) | ❌ | |
| Ray tracing com geometria dinâmica (carros e pessoas se movendo) | ❌ | Hoje o TLAS é estático |

## Além do gráfico

O RAGE é um motor de jogo inteiro, não só gráfico. Estes blocos também fazem parte do GTA 6, mas são projetos separados do motor gráfico:

- **Física:** veículos, rigid bodies, ragdoll procedural (Euphoria) e destruição.
- **Animação:** blending, IK, motion matching e locomoção procedural.
- **IA:** navmesh, trânsito e rotinas de NPC.
- **Áudio 3D e scripting de missões.**
- **Ferramentas:** editor de mapa e pipeline de assets.

## Ordem sugerida

Cada etapa destrava a seguinte.

1. **Base temporal:** motion vectors, TAA, denoiser e upscaling. Todo o ray tracing depende disso.
2. **Assets reais:** glTF, texturas, normal maps, bindless, BC7. Sem isso não existe cidade.
3. **Pós-processamento cinematográfico:** bloom, auto-exposição, profundidade de campo e motion blur. É o maior ganho visual por esforço.
4. **Muitas luzes (ReSTIR DI):** a noite de Vice City.
5. **Cena dinâmica:** TLAS atualizado por frame e skinning de personagens.
6. **Escala:** GPU-driven, culling, LOD e streaming.
7. **Atmosfera viva:** nuvens volumétricas, clima, chuva, superfícies molhadas.
8. **Água:** oceano FFT, rios e cena subaquática.
9. **Personagens AAA:** pele com subsurface scattering, cabelo por fios e tecido.

## Fontes

- [Tom's Guide — o que se sabe de GTA 6](https://www.tomsguide.com/us/grand-theft-auto-6-release-date-rumors,news-25578.html)
- [Fox10 — data de 19/11/2026 confirmada](https://www.fox10tv.com/2026/10/16/gta-6-locks-nov-19-release-date-after-delays/)
- [GameSpot — Trailer 2 a 30 FPS](https://www.gamespot.com/articles/gta-6-trailer-2-was-only-30fps-and-it-may-be-too-much-for-switch-2/1100-6531393/)
- [Notebookcheck — Trailer 2 capturado em PS5](https://www.notebookcheck.net/PS5-was-behind-every-frame-of-GTA-6-Trailer-2-confirms-Rockstar.1012400.0.html)
- [LibertyCity — resumo da análise da Digital Foundry](https://libertycity.net/news/gta-6/8997-digital-foundry-analyzes-gta-6s-second.html)
- [GameGPU — ray tracing, 1440p a 30 FPS no PS5](https://en.gamegpu.com/news/igry/gta-6-ispolzuet-trassirovku-luchej-i-1440p-pri-30-fps-na-bazovoj-playstation-5)
- [GTA6 Blueprint — sombras e GI](https://gta6blueprint.com/guides/gta-6-graphics-tech)
- [Press Start — patente de animação](https://press-start.com.au/news/2023/10/30/gta-6-looks-to-be-getting-super-realistic-animation-tech-in-a-newly-discovered-rockstar-patent/)
- [GameGPU — patentes da Take-Two](https://en.gamegpu.com/news/igry/gta-6-mozhet-poluchit-realistichnuyu-animatsiyu-ii-povedenie-npc-i-mgnovennye-zagruzki-najdeno-bolee-7-patentov-ot-take-two)
