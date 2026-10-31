# O que tem no motor do GTA 6 e o que falta no Legacy Engine

Pesquisa de 22/10/2026, status revisado pela auditoria do Dia 4 (ver [auditoria.md](auditoria.md)): um item só é ✅ se passar no teste de qualidade. O GTA 6 está previsto para 19/11/2026 (PS5 e Xbox Series).

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
| GI por ray tracing | ✅ | 1 rebote traçado + cache de radiância no espaço do mundo (infinitos rebotes); sala de teste contra path tracing: viés −64% → −4% |
| Reflexos por ray tracing | ✅ | 100% ray tracing (inclui fora da tela), reflexo do reflexo em espelhos, luz indireta pelo cache, movimento virtual no denoiser; erro 0,068 contra path tracing |
| Sombra do sol | ✅ | Ray tracing com penumbra física do disco solar; sem acne e sem degraus no terminator (visibility buffer + Hanika 2021 + Chiang 2019, medido em `tools/sombras.py`) |
| Shadow maps em cascata (fallback e performance) | ✅ | 4 cascatas 2048² (divisão prática λ = 0,75, esfera envolvente estável, passos de um texel), PCF por comparação em hardware, folhagem com recorte e vento; sempre sem ray tracing, e com `--shadow-maps` numa RTX (imagem praticamente igual à sombra traçada na Sponza). Sem RT a luz ambiente ainda não tem oclusão |
| Milhares de luzes (neon, faróis, postes) | ✅ | ReSTIR de produção: BVH de luzes, GRIS/MIS em pares, correção de viés por ray tracing, filtro de boiling; viés < 1,5% contra força bruta (`tools/restir.py`) |
| Céu e atmosfera física | ✅ | Pipeline completa de Hillaire (transmitância, múltiplo espalhamento, sky-view, perspectiva aérea), 4% de erro contra path tracing de referência |
| Nuvens volumétricas, raios de luz (god rays) | ✅ | Nuvens Nubis/Hillaire (Perlin-Worley, múltiplo espalhamento, vento) no céu, nos reflexos e com sombra no chão; névoa volumétrica em froxels com sombra do sol por ray tracing e céu com visibilidade (raios de luz, névoa escura dentro de prédios). Nuvens medidas contra Monte Carlo volumétrico com espalhamento múltiplo exato (`tools/nuvens.py`): viés −5,7%, correlação 0,83 (antes do ajuste: +118% e 0,27) |
| Ciclo dia/noite | ✅ | Sol físico; à noite lua (luz e disco), estrelas e luzes da cidade (milhares pelo ReSTIR + emissivos como luz) |

### Imagem

| Recurso | Legacy | Observação |
|---|---|---|
| Denoiser | ✅ | DLSS Ray Reconstruction (padrão na RTX; o mesmo do Cyberpunk 2077 path traced): medido na RTX 3070 Ti contra a referência verdadeira, erro 0,108 andando (SVGF 0,163, ReLAX 0,211), cintilação 2,3x menor; SVGF/NRD do motor sem RTX |
| TAA | ✅ | Reconstrução gaussiana medida contra referência 4x (nitidez 0,83 → 0,95) + nitidez RCAS da AMD |
| Upscaling (FSR, DLSS, XeSS) | ✅ | DLSS 4 (padrão em RTX) e FSR 3 integrados, com volta automática ao TAAU próprio. Medido na RTX 3070 Ti: DLSS em 67% erra 20% menos que o TAAU contra a referência; XeSS no Vulkan não existe (só D3D12) |
| Pós-processamento: bloom, profundidade de campo, motion blur, auto-exposição, lens flare | ✅ | Bloom físico, DOF com autofoco, motion blur, auto-exposição por histograma, ACES; reflexo nas lentes (fantasmas espelhados, halo, dispersão cromática, sujeira na lente, `--no-lens-flare`) |

### Materiais e superfícies

| Recurso | Legacy | Observação |
|---|---|---|
| PBR | ✅ | Unidades físicas, compensação de energia (teste da fornalha), especular/glossiness, KHR specular/ior, AA especular |
| Texturas, normal maps, mipmaps, compressão BC7 | ✅ | BC7 na cor, BC5 nos normal maps (padrão da indústria), mips na CPU, cache: 4x menos memória de vídeo, PSNR médio 46,8 dB, imagem final a 1,6% do RGBA8 |
| Importar assets profissionais (glTF completo) | 🟡 | DDS, luzes do glTF, luzes de malha (energia provada: −0,3% / +0,02% contra path tracing ingênuo), câmera, samplers, doubleSided, oclusão, cobertura alfa, vidro (transmissão e volume), verniz, BLEND, `KHR_texture_transform`, cor por vértice `COLOR_0` (raster, mesh shader, raios e sombra; passa no VertexColorTest da Khronos); falta só MikkTSpace de referência (aguarda aprovação para baixar a biblioteca) |
| Pintura de carro (clearcoat, flocos metálicos) | ✅ | `KHR_materials_clearcoat` (verniz com reflexo traçado próprio) + flocos metálicos que viram rugosidade à distância (sem cintilar) |
| Vidro e transparência | ✅ | Vidro por ray tracing: parede fina (janelas, vitrines, fosco, várias lâminas) e com volume (`KHR_materials_volume`: refração de Snell, reflexão interna total, absorção de Beer), sombra colorida pelo vidro; `BLEND` do glTF misturado no G-buffer (decals de sujeira e rachaduras do Bistro). Limite: objeto translúcido genérico por mistura (não vidro) recebe a luz da superfície dele, não a do que está atrás |
| Decals (sujeira, marcas, pichação) | ✅ | Decals por caixa projetada aplicados no material (aparecem nos reflexos e molham com a chuva): procedurais de chão (faixa de pedestre, tracejado, sujeira, óleo, rachaduras) e de imagem em qualquer superfície virada para a caixa (pichação e cartazes nos muros da cidade); BLEND do glTF misturado no G-buffer. Limite: laço sobre todos os decals por pixel (uma grade espacial entra quando houver milhares) |
| Chuva, superfícies molhadas e poças | ✅ | Molhado por porosidade, poças com reflexo por ray tracing e anéis dos pingos, pingos caindo e respingos no chão (partículas), céu fechado; abrigo sob telhados por um mapa de oclusão traçado de cima (`--rain`). Sem ray tracing, molha tudo |
| Água: oceano FFT, ondas, rios, espuma, cena subaquática | ✅ | Oceano FFT de Tessendorf (256², Phillips com vento, ondas "choppy", espuma pelo Jacobiano, crista translúcida, reflexo por ray tracing); vista subaquática com absorção de Beer por canal (água tipo Jerlov I), janela de Snell e reflexão interna total vistas de baixo, sol filtrado pela profundidade e cáusticas pelo foco das ondas (`--scene ocean`). Rios (`--scene river`): água corrente numa malha comum, detalhe arrastado pela correnteza em duas fases (flow map, Vlachos 2010) na direção da tangente — medido de cima: o padrão anda rio abaixo. Água empurrada por objetos: equação da onda numa grade de 64 m em volta da câmera, objetos que andam cruzando a superfície afundam a água sob eles e levantam em volta (esteira em V e anéis do barco no rio e da boia no mar; `--no-ripples`). As ondulações mudam a normal e também a altura dos vértices da água (mar e rio, malha do rio de 1 m). Limite: sem espuma de esteira |

### Personagens

| Recurso | Legacy | Observação |
|---|---|---|
| Skinning (esqueleto) | ✅ | Skinning em compute + refit de BLAS (Dia 3) |
| Pele com subsurface scattering, olhos | ✅ | Espalhamento subsuperficial separável (Jimenez 2015) com raio por canal em metros, transmissão em partes finas contra a luz (espessura e saída por ray tracing) e olho procedural (íris com fibras, anel límbico, pupila, esclera com veias, córnea com reflexo nítido) — `--scene skin`, `skin-backlit`. Falta um personagem com pele de verdade (scan) para comparar |
| Cabelo por fios | ✅ | Fios de verdade (14 mil × 14 segmentos, `--scene hair`): fitas viradas para a câmera, transparência estocástica abaixo de um pixel (o TAA/DLSS resolve), modelo de Marschner nas aproximações de Karis/Unreal (R, TT, TRT, absorção pela cor), sombra e autossombra por ray tracing, física por fio na GPU (Follow-The-Leader dinâmico, Müller 2012): gravidade, vento, retorno ao penteado, comprimento exato e colisão com o couro; a cabeça vira e o cabelo atrasa e balança (`tools/cabelo.py`: erro de comprimento 0,000%, nenhum ponto dentro da cabeça, desvio da pose rígida de 0 mm na raiz a 14 mm na ponta). Limite: sem colisão fio com fio |
| Tecido simulado | ✅ | Verlet com restrições (PBD) na GPU, um grupo de trabalho por pano (até 1.024 pontos): gravidade, vento com rajadas, borda presa ou pano solto, colisão com esferas e atrito; sombras e reflexos acompanham (refit da BLAS). Bandeira na cena externa, toalha caindo sobre uma bola (`--scene cloth`). Colisão do pano consigo mesmo (todos os pares não vizinhos a ≥ 0,8 do espaçamento): toalha caída, `tools/tecido.py`: sem ela 16 pares atravessando (menor distância 0,37), com ela nenhum (0,79) |
| Multidões (centenas de NPCs animados) | ✅ | Animação assada (32 poses do ciclo) lida no vertex shader por pessoa, sem skinning individual; no ray tracing cada pessoa aponta para a BLAS da sua pose (sombras e reflexos certos); culling na GPU. 300 pessoas: 1,6 ms de CPU por quadro (`--scene crowd`). LOD das poses (1/4 e 1/8 dos triângulos, o mesmo agrupamento de vértices em todas as 32 poses, troca a ~45 e ~125 m): 3.000 pessoas com G-buffer −70% (3.774 → 1.144 ms no llvmpipe); 300 pessoas: 0,24% dos pixels mudam (média 0,33 de 255). Limite: sem impostores (de longe ainda são malhas) |

### Mundo aberto

| Recurso | Legacy | Observação |
|---|---|---|
| Streaming do mundo | ✅ | Streaming de geometria por células: malhas e BLAS sobem para a GPU perto da câmera e saem longe (histerese, orçamento por frame, destruição adiada para os frames em voo), TLAS compacto só com o que está na memória. `--scene stream`: 1.600 torres únicas (4,4 M triângulos), ~126 residentes (~90 MB) andando a cidade. As malhas das células ficam só no disco (um arquivo de pacote escrito no carregamento) e uma thread de fundo lê cada célula pedida; a principal só sobe o que já foi lido (o 1º frame espera tudo ao alcance, como uma tela de carregamento). Igual ao streaming antigo com `--stream-sync` (diferença 0); assíncrono andando a 200 m/s: média ≤ 0,9 de 255 (células na borda chegando um frame depois). Streaming de texturas: os mips das texturas comprimidas ficam num arquivo, a GPU começa com os níveis de até 128 texels e o G-buffer e os raios anotam a resolução que precisam de cada textura; os níveis finos sobem com orçamento por frame e descem após ~5 s sem uso. Bistro: 7 MB no início, 1.266 MB depois de convergir (contra 1.930 MB com tudo), imagem igual a menos do ruído (média 0,26 de 255, sem região borrada). Cópias para a GPU e BLAS das células e das texturas vão num lote enviado sem esperar (a thread principal não fica parada): `stream()` caiu de 2.709 para 50 ms somados em 40 frames andando a 200 m/s, imagem idêntica |
| LOD e HLOD de cidade inteira | ✅ | LOD automático: malhas com mais de 3 mil triângulos ganham 2 níveis (1/4 e 1/16) por agrupamento com erro quádrico (Lindstrom), trocados por distância no culling da GPU (ray tracing segue com a malha completa); Bistro: imagem praticamente igual (0,3% dos pixels). HLOD: objetos estáticos agrupados por célula (128 m) e material viram um proxy fundido (simplificado quando grande), trocado a 250 m da caixa da célula; ray tracing e sombras continuam com os objetos de verdade. Cidade 30×30: 3.540 proxies no lugar de 20.895 objetos ao longe (1,22 M → 0,35 M triângulos), imagem de cima igual (média 0,04 de 255). Troca suave por pontilhado complementar na faixa de 10% em volta de cada distância (LOD e HLOD), sorteado a cada frame para o TAA/DLSS misturar: andando, nenhum quadro diverge dos objetos de verdade (média ≤ 0,05). Ganho na RTX: `teste_pc.py --so gpudriven` |
| Renderização GPU-driven (culling em compute, oclusão, meshlets) | ✅ | Culling por frustum em compute (uma invocação por instância, caixa transformada) com desenho indireto por malha; oclusão em duas fases pela pirâmide de profundidade (HiZ: desenha o visível do frame anterior, monta a pirâmide, testa todos e desenha os que apareceram); meshlets por mesh shader (`VK_EXT_mesh_shader`, 64 vértices / 124 triângulos, na ordem original) com descarte por meshlet no task shader (fora da tela e cone de normais de costas). Imagem igual com e sem cada um: oclusão diferença 0 (Sponza parada e cidade andando); meshlets só arredondamento (≤ 0,1 nível em média; nenhum objeto falta). Ganho na RTX: `teste_pc.py --so gpudriven` |
| Vegetação instanciada com vento | ✅ | Grama procedural (tufos de lâminas curvas) instanciada aos milhares (uma chamada e uma BLAS por variação), folhagem dos assets e grama balançam com rajadas, curvatura pela altura e tremor das folhas, com vetores de movimento certos. Falta a sombra traçada seguir o balanço (BLAS parada) |
| Terreno grande (clipmaps) | ✅ | Mundo de 8 × 8 km (`--scene world`): blocos de 256 m a 4 m de resolução carregados pelo streaming até ~1,2 km da câmera, mais uma malha grossa do terreno inteiro (62 m) para o horizonte — no raster e no ray tracing (as montanhas fazem sombra ao longe). Em vez de clipmaps de GPU, a mesma solução de blocos do World Partition do Unreal. Terreno por camadas: grama, terra, rocha e cascalho com cor+altura e normal map (texturas geradas no carregamento, repetidas sem emenda; qualquer textura real entra no lugar), peso por inclinação e ruído, mistura por altura (a pedra aparece entre a grama), triplanar nos barrancos e normal da camada sobre a da geometria; nos raios só a cor planar. Custo: G-buffer +50% na cena externa (llvmpipe), luz igual |
| Partículas e VFX (fogo, fumaça, explosões) | ✅ | Partículas sem estado na GPU: fogo de corpo negro com luz que tremula, fumaça iluminada (sol com sombra traçada, céu), faíscas, explosões (bola de fogo que esfria, fuligem, fragmentos, clarão de luz), chuva e respingos; soft particles (`--scene vfx`). Limite: sem máscara reativa para o DLSS/TAA (rastro leve em partículas rápidas) |
| Ray tracing com geometria dinâmica (carros e pessoas se movendo) | ✅ | TLAS por frame, BLAS deformáveis (Dia 3) |

## Além do RAGE: diferenciais planejados

Paridade é o piso. Estes recursos dariam ao Legacy vantagem sobre o motor do GTA 6:

- **Modo path tracing completo** (como o RT Overdrive do Cyberpunk), alternável em tempo real com o modo híbrido.
- **ReSTIR GI + cache de radiância neural**: iluminação global multi-rebote com custo de 1 rebote.
- **Geometria virtualizada** (estilo Nanite): bilhões de triângulos sem LOD manual.
- **Gaussian splatting** integrado à cena com ray tracing: capturas fotorreais do mundo real como asset.
- **Nuvens, neblina e fumaça volumétricas com ray tracing**: sombras e luz atravessando o volume.
- **Câmera física completa**: abertura, ISO, obturador e bokeh com formato de lâmina real.
- **Modo foto de cinema**: path tracing com acumulação para screenshots de qualidade de render offline.

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
