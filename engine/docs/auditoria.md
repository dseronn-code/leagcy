# Auditoria completa — Dias 1 a 3

Revisão linha a linha de todo o código (C++ e GLSL), testes na cena **Bistro** (NVIDIA/Amazon Lumberyard,
licença MIT, 1,75 M triângulos, 2.909 objetos, 254 materiais, 97 luzes) e comparação com o estado da arte.

Gravidade: **🔴 bug** (resultado errado) · **🟠 qualidade** (funciona, mas visivelmente abaixo do topo) ·
**🟡 não é o melhor disponível** (existe técnica/ferramenta superior e gratuita).

---

## 1. Causas dos "brilhinhos" (prioridade máxima)

| # | Gravidade | Problema | Evidência | Solução de ponta |
|---|---|---|---|---|
| 1.1 | 🔴 | Momentos de variância do denoiser em float16: luminância² estoura 65504 com luminância > ~256 → `inf`/`NaN` que se espalham | `svgf_temporal.comp` grava `moments` em RGBA16F | Momentos em RGBA32F; sinal com **pré-exposição** (Frostbite) |
| 1.2 | 🔴 | Especular do sol estoura float16: D(GGX) com rugosidade 0,03 ≈ 3,9×10⁵ → ×sol = 7,8×10⁴ (dielétrico) a 2×10⁶ (metal) | Cálculo direto; `outSpecular` é RGBA16F | Pré-exposição + sol como **luz de área** (disco) no BRDF, sem pico infinito |
| 1.3 | 🔴 | Lâmpada (esfera emissiva) bloqueia a própria luz: raio de sombra termina na superfície da esfera → ora acerta, ora não | Esfera e luz com o mesmo raio em `demo_scene.cpp` | Luzes como **geometria emissiva amostrada** (mesh lights, como no RTXDI) ou máscara de instância |
| 1.4 | 🔴 | Energia contada 2×: a esfera emissiva aparece na GI **e** a luz pontual também ilumina | Mesmo caso acima | Idem 1.3 (uma única representação da luz) |
| 1.5 | 🔴 | Ponto sorteado na luz usa `rand2().xyx` → pontos numa diagonal, não numa esfera; pdf errada | `lighting.glsl:87` | Amostragem por ângulo sólido da esfera (cone) com pdf correta |
| 1.6 | 🔴 | Gerador aleatório por combinação linear de (x, y, frame): padrões correlacionados entre pixels e frames | `lighting.glsl:65`, `restir_*.glsl` | Hash robusto + **ruído azul espaço-temporal (STBN, NVIDIA)** / Sobol embaralhado |
| 1.7 | 🟠 | Luzes candidatas sorteadas uniformemente entre todas: com 512 luzes quase todas fora do alcance → p̂ = 0 → ruído | `restir_temporal.glsl:27` | Amostragem por potência + **ReGIR** (grade de reservatórios do RTXDI) |
| 1.8 | 🟠 | Luz local aleatória nos pontos de GI (uniforme, sem alcance) → altíssima variância | `lighting.glsl:43` | ReGIR / ReSTIR GI |
| 1.9 | 🟠 | Reuso espacial do ReSTIR enviesado, sem visibilidade nem MIS → escurecimento e "fervura" (boiling) | `restir_spatial.comp` | **Pairwise MIS** + raio de visibilidade + filtro de boiling (RTXDI) |
| 1.10 | 🟠 | Aliasing especular: sem *specular AA* geométrico, mips de normal map sem filtragem de rugosidade → cintilação a distância | `material.glsl`, `resources.cpp` (blit linear) | Specular AA (Kaplanyan/Tokuyoshi) + rugosidade a partir da variância das normais nos mips |
| 1.11 | 🟠 | Origem dos raios deslocada pela **normal de shading** (com normal map) e 1 cm fixo → auto-interseção ("acne") e vazamento | `lighting.glsl:67`, `raytracing.glsl` | Normal **geométrica** no G-buffer + offset robusto (Wächter & Binder, Ray Tracing Gems cap. 6) |
| 1.12 | 🟠 | Corte fixo de "vagalumes" em 16 (radiância absoluta) — viés dependente da cena | `lighting.glsl:19` | Supressão relativa no denoiser + amostragem melhor (1.7–1.9) |
| 1.13 | 🟡 | Denoiser SVGF básico: sem reprojeção de reflexo (movimento virtual), sem anti-firefly, especular com só 2 iterações | `svgf_*.comp` | **NVIDIA NRD (ReBLUR/ReLAX + SIGMA)** e, em RTX, **DLSS Ray Reconstruction** |

## 2. Assets profissionais não funcionam (comprovado no Bistro)

| # | Gravidade | Problema | Evidência | Solução |
|---|---|---|---|---|
| 2.1 | 🔴 | Texturas DDS/BC7 não suportadas → **0 de 343** texturas carregadas | Log: 129 avisos "Textura não carregada" | Leitor DDS/KTX2 com formatos BCn nativos (`MSFT_texture_dds`, `KHR_texture_basisu`) |
| 2.2 | 🔴 | `KHR_lights_punctual` ignorado → **0 de 97** luzes | Log: "0 luzes" | Ler luzes pontuais, spot e direcionais do glTF |
| 2.3 | 🔴 | `KHR_materials_pbrSpecularGlossiness` ignorado → materiais brancos | 254 materiais do Bistro usam esse modelo | Conversão especular/glossiness → metálico/rugosidade (ou suporte nativo) |
| 2.4 | 🔴 | Vidro (`KHR_materials_transmission`) tratado como opaco → janelas bloqueiam a cena | 18 materiais de vidro | Transmissão/refração por ray tracing |
| 2.5 | 🔴 | Unidades arbitrárias (sol = "20"): luzes de assets em lux/candela não batem | Sol do Bistro = 6.830 lux | **Unidades fotométricas** (lux, candela, nits) + exposição EV100 + pré-exposição |
| 2.6 | 🔴 | `doubleSided` do glTF ignorado (tudo é desenhado dos dois lados) | `renderer.cpp`: `cullMode = NONE` | Respeitar `doubleSided`; pipelines separados |
| 2.7 | 🔴 | Tangentes geradas sem **MikkTSpace** (obrigatório pelo glTF) → costuras em normal maps | `mesh.cpp: computeTangents` | Biblioteca MikkTSpace (zlib) |
| 2.8 | 🔴 | Ignorados: samplers do glTF (wrap/filtro), `COLOR_0`, `KHR_texture_transform`, `occlusionTexture`, câmeras | `gltf_loader.cpp` | Implementar cada um |
| 2.9 | 🟠 | Mips de texturas com alfa perdem cobertura → folhagem some a distância | `createTexture` | Mips com preservação de cobertura alfa (Castaño) |
| 2.10 | 🟠 | Texturas sem compressão (RGBA8): 4–8× mais VRAM — a RTX 3070 Ti tem 8 GB | `createTexture` | BC7/BC5/BC4 (DDS/KTX2), streaming de mips |

## 3. Iluminação e materiais abaixo do topo

| # | Gravidade | Problema | Solução de ponta |
|---|---|---|---|
| 3.1 | 🟠 | GI de 1 rebote + "céu sem oclusão" como 2º rebote → vazamento de luz em interiores | Cache de radiância (SHaRC / ReSTIR GI) multi-rebote |
| 3.2 | 🟠 | Pontos atingidos por raios ignoram o especular (metais aparecem pretos em reflexos/GI) | BRDF completo no hit |
| 3.3 | 🟠 | LOD de textura fixo (mip 2) nos hits | Ray cones (Akenine-Möller) |
| 3.4 | 🟠 | BRDF sem compensação de múltiplo espalhamento → metais rugosos escuros demais | Kulla-Conty / Fdez-Agüera |
| 3.5 | 🟡 | Modelo de material só metálico/rugosidade básico | **OpenPBR Surface** (padrão ASWF 2024) + extensões KHR (clearcoat, sheen, transmission, volume, iridescence, anisotropy) |
| 3.6 | 🟠 | Atmosfera: transmitância por raymarch em cada chamada; múltiplo espalhamento é uma constante "chutada" | Hillaire 2020 completo: LUT de transmitância + LUT de múltiplo espalhamento + perspectiva aérea em volume |
| 3.7 | 🟠 | Névoa analítica sem sombras (sem god rays) | Névoa volumétrica em froxels com sombras por ray tracing |
| 3.8 | 🔴 | Sem ray tracing (Iris Xe): **nenhuma sombra** | Shadow maps (cascatas/virtual) + GTAO como fallback |
| 3.9 | 🟡 | Tonemapper ACES "fitted" (desvio de matiz) | **AgX** (padrão Blender) com opção Khronos PBR Neutral; saída **HDR10** |

## 4. Imagem final

| # | Gravidade | Problema | Solução de ponta |
|---|---|---|---|
| 4.1 | 🟠 | TAAU usa a amostra bilinear da resolução interna → imagem "mole" | DLSS (RTX), XeSS (Intel — roda no Iris Xe), FSR 3.1 (todas); TAAU próprio com reconstrução por kernel + nitidez RCAS |
| 4.2 | 🟠 | Motion blur sem *tile max / neighbor max* → borrão cortado na silhueta do objeto | McGuire 2012 / Jimenez (COD) |
| 4.3 | 🟡 | DOF por gather simples | DOF com bokeh por scatter/gather (estilo UE "Diaphragm DOF") |

## 5. Arquitetura e desempenho

| # | Gravidade | Problema | Solução de ponta |
|---|---|---|---|
| 5.1 | 🟠 | Barreira global entre todos os passes, imagens sempre em `GENERAL` | Render graph com barreiras precisas + compute assíncrono |
| 5.2 | 🟠 | Sem culling: 2.909 objetos desenhados todo frame (4,2 M triângulos) | Renderização GPU-driven com culling por frustum/oclusão |
| 5.3 | 🟠 | Upload: um submit + espera por textura/buffer/BLAS; texturas com memória dedicada | Uploads em lote + fila de transferência; VMA suballocation |
| 5.4 | 🟠 | BLAS sem compactação (≈ 2× memória) | Compactação + construção em lote |
| 5.5 | 🟠 | Alfa recortado e opaco no mesmo pipeline (`discard` desliga early-Z para tudo) | Pipelines separados |
| 5.6 | 🟡 | SPIR-V sem otimização | `spirv-opt -O` |

---

## Plano de correção (um item por vez, cada um com análise completa antes de contar como feito)

1. ✅ **Base numérica e física**: unidades fotométricas + pré-exposição + float32 onde precisa (1.1, 1.2, 2.5) — ver resultado abaixo
2. ✅ **Ruído e amostragem**: Sobol-Owen, amostragem correta da esfera, offset robusto com normal geométrica (1.5, 1.6, 1.11)
3. **Luzes reais**: luzes como geometria emissiva + `KHR_lights_punctual` (1.3, 1.4, 2.2)
4. **Assets profissionais**: DDS/BC7, especular/glossiness, MikkTSpace, samplers, doubleSided, alfa (2.1, 2.3, 2.6–2.10)
5. **ReSTIR de produção**: ReGIR, pairwise MIS, visibilidade, filtro de boiling (1.7–1.9)
6. **Denoiser de ponta**: NRD (ReBLUR/ReLAX/SIGMA) + specular AA (1.10, 1.13)
7. Itens 3–5 restantes

Fontes da pesquisa: [Intel GPU Research Samples](https://www.intel.com/content/www/us/en/developer/topic-technology/graphics-research/samples.html) ·
[RTXDI assets (Bistro)](https://github.com/NVIDIAGameWorks/rtxdi-assets) · [niagara_bistro](https://github.com/zeux/niagara_bistro) ·
[NRD](https://developer.nvidia.com/nrd) · [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) ·
[Streamline/DLSS](https://developer.nvidia.com/rtx/streamline/get-started) · [Hillaire 2020](https://diglib.eg.org/handle/10.1111/cgf14050) ·
[A Gentle Introduction to ReSTIR](https://par.nsf.gov/servlets/purl/10519651) · [Tone mapping overview](https://discourse.threejs.org/t/tone-mapping-overview/75204)

---

## Registro de correções

### Correção 1 — unidades fotométricas, pré-exposição e precisão numérica

O que mudou:
- Unidades físicas: sol em lux (120.000 no topo da atmosfera), luzes em candela, emissão em nits, câmera em EV100
  (Lagarde & de Rousiers, *Moving Frostbite to PBR*), calibrada para cinza médio de 18%.
- Pré-exposição: toda radiância é multiplicada pela exposição do frame anterior antes de ir a um alvo float16;
  históricos do denoiser e do TAA são reescalados quando a exposição muda; saturação segura em 60.000.
- Momentos do denoiser em float32.
- Especular do sol como disco (rugosidade equivalente de Karis): sem o pico de um sol pontual.

Medição A/B (mesma cena, câmera parada, 16 frames, brilho igualado; métrica em `flicker.py`):

| Cena | Cintilação média | Pior 1% | Pixels com brilhinho |
|---|---|---|---|
| Sponza noite | 0,0695 → 0,0550 (−21%) | 0,650 → 0,488 (−25%) | 2,58% → 1,34% (−48%) |
| Sponza dia | 0,0291 → 0,0210 (−28%) | 0,241 → 0,145 (−40%) | 0,45% → 0,26% (−42%) |
| Externa | 0,0175 → 0,0165 (−6%) | 0,463 → 0,465 (0%) | 1,14% → 1,10% (−4%) |

Interferências verificadas: validação Vulkan com sincronização limpa em 7 configurações (com e sem ray tracing,
DOF, sem denoiser, exposição manual); de dia as luzes decorativas (36–150 cd) deixam de dominar a cena, como na
realidade. A cena externa melhora pouco porque sua causa principal é a 1.3 (lâmpada bloqueando a própria luz).

### Correção 2 — ruído, amostragem e robustez dos raios

O que mudou:
- Gerador: hash PCG3D (Jarzynski & Olano 2020) no lugar de combinações lineares; amostras de integração com
  **Sobol + embaralhamento de Owen por hash** (Burley 2020, usado no RenderMan): sequência de baixa discrepância
  por pixel ao longo dos frames. *O STBN da NVIDIA foi descartado por ter licença não comercial; as texturas FAST
  da EA (BSD) ficam como melhoria quando puderem ser geradas no Windows.*
- Luzes esféricas amostradas por ângulo sólido com pdf correta (antes: pontos numa diagonal); removida a
  distância mínima artificial de 25 cm.
- Origem dos raios com a **normal geométrica** (reconstruída da profundidade; nos hits, da faceta do triângulo)
  e deslocamento de Wächter & Binder; direções abaixo da faceta são descartadas (sem vazamento de luz).
- Normal de shading corrigida para o lado visível antes de qualquer cálculo de luz (antes só depois do sol).
- Névoa por cena: sem sombra volumétrica, a névoa era iluminada pelo sol dentro das áreas de sombra e cobria
  interiores com um véu branco (achado durante esta análise; ver 3.7). Sponza agora usa densidade baixa.

Medição honesta (cena congelada com `--pause`, 16 frames):

| Cena | Correção 1 | Correção 2 |
|---|---|---|
| Sponza noite (com denoiser) | 0,0458 / 0,64% | 0,0471 / 0,63% |
| Sponza noite (sem denoiser) | 0,1037 / 1,89% | 0,1077 / 1,95% |
| Externa (com denoiser) | 0,0045 / 0,00% | 0,0044 / 0,01% |

Conclusões: (1) efeito neutro na cintilação — o valor desta correção é exatidão física e robustez, não redução de
ruído; (2) **com a cena parada a externa praticamente não cintila**: os "brilhinhos" dela vinham de objetos em
movimento (tarefa do denoiser, correção 6); (3) o ruído noturno restante vem da escolha de luzes do ReSTIR
(correção 5). Validação com sincronização limpa em 7 configurações.

### Correção 2b — orçamentos de qualidade escaláveis (presets)

Os orçamentos por pixel estavam fixos no código (resolução interna de 67%, 1 amostra por pixel, 8 candidatos do
ReSTIR, 2 passadas do denoiser no especular, texturas no mip 2 nos raios, sol 2,5x maior que o real). Agora vêm de
presets — **Baixo, Médio, Alto, Ultra (padrão) e Cinema** — enviados aos shaders a cada frame. O sol passou a ter o
tamanho angular real (0,00465 rad).

| Preset | Resolução interna | Amostras/pixel | Candidatos ReSTIR | Vizinhos | Denoiser (dif./esp.) | Mip nos raios |
|---|---|---|---|---|---|---|
| Baixo | 50% | 1 | 4 | 3 | 3 / 1 | 3 |
| Médio | 60% | 1 | 8 | 4 | 4 / 2 | 2 |
| Alto | 75% | 1 | 12 | 6 | 5 / 3 | 1 |
| **Ultra** | **100%** | **2** | **16** | **8** | **5 / 4** | **0** |
| Cinema | 100% | 4 | 32 | 12 | 5 / 5 | 0 |

Medição (Sponza noite, cena congelada; antes = padrão anterior com cintilação 0,0471 e 0,63% de brilhinhos):
Alto 0,0393 / 0,45% · **Ultra 0,0307 / 0,27% (−57% de brilhinhos)** · Cinema 0,0272 / 0,16% (−75%).
Validação com sincronização limpa nos 5 presets × 4 configurações (20 casos). Desempenho a medir na RTX 3070 Ti.

### Item 1 recuperado — PBR de ponta ✅

O que mudou:
- **Compensação de múltiplo espalhamento** (Turquin 2019, como Filament/UE5) com LUT DFG pré-computada na GPU
  (amostragem VNDF, peso exato G2/G1 do Smith "height-correlated" — o mesmo termo do BRDF).
- **Difuso com conservação de energia** ("albedo scaling" do OpenPBR): a fração refletida pelo especular sai do
  difuso, no lugar de (1 − Fresnel).
- **G-buffer com cor difusa + f0/f90**: suporte nativo a especular/glossiness (Bistro), `KHR_materials_specular` e
  `KHR_materials_ior`, sem conversão com perda.
- **Anti-aliasing especular**: Tokuyoshi & Kaplanyan 2019 (variação da normal no pixel) + Toksvig (variância do
  normal map nos mips).
- **BRDF completo nos pontos atingidos por raios** (difuso + especular + reflexo do céu): metais deixam de ficar
  pretos em reflexos e na GI.

Validação — **teste da fornalha branca** (`--scene furnace`, `tools/furnace.py`): esferas brancas sob céu uniforme
devem ter o brilho do fundo (1,00). Valores no espaço da tela, rugosidade 0 → 1:

| Material | Sem compensação | Com compensação |
|---|---|---|
| Metal f0 = 1 | 0,98 · 1,00 · 0,93 · 0,74 · 0,41 · **0,16** | 0,98 · 1,00 · 0,98 · 0,98 · 0,99 · **0,94** |
| Dielétrico branco | 0,96 · 0,98 · 0,96 · 0,96 · 0,95 · 0,90 | 0,96 · 0,98 · 0,96 · 0,96 · 0,98 · 0,95 |

Perda residual de 2–6%: raios que atingem esferas vizinhas usam a aproximação de céu do 2º rebote (auditoria 3.1,
a ser resolvida com o cache de radiância). Cintilação (Sponza noite, congelada): 0,27% → 0,24% de brilhinhos;
dia 0,02%. Validação com sincronização limpa (fornalha, Sponza dia/noite, externa com DOF, sem RT, Bistro).
Fora deste item: difuso EON/rugosidade difusa (o glTF não tem o parâmetro), clearcoat, sheen e transmissão
(itens "pintura de carro" e "vidro").

### Item 2 recuperado — atmosfera de Hillaire completa ✅

O que mudou (Hillaire, EGSR 2020, mesma técnica do UE4/UE5):
- **LUT de transmitância** 256×64 (parametrização de Bruneton & Neyret), calculada uma vez na inicialização.
- **LUT de múltiplo espalhamento** 32×32 (16×16 direções, 40 passos, chão Lambert com albedo 0,3): soma de
  todas as ordens pela série geométrica Ψ = L₂ / (1 − f_ms). Substitui a constante "0,08" de antes.
- **Sky-view LUT** com 96 passos e termo do chão abaixo do horizonte.
- **Perspectiva aérea** em volume 32³ alinhado ao frustum (até 32 km, fatias quadráticas): montanhas distantes
  ficam azuladas de dia e alaranjadas no pôr do sol. Aplicada no composite antes da névoa por altura.
- Sombra da Terra com transição suave no crepúsculo; `--sun-elevation`/`--sun-azimuth` para testar qualquer hora.

Validação — **`--atmosphere-test N`**: path tracer de referência na GPU (todas as ordens de espalhamento, fases
Rayleigh/Mie exatas, chão Lambert) contra o céu do motor em 30 casos (sol a 60°, 20°, 5°, 1° e −2°; seis
direções de visão cada). Erro médio de luminância **4,05%** (só espalhamento simples: 15–51% abaixo). O resíduo
é a hipótese de espalhamento isotrópico das ordens ≥ 2 do próprio método (~0,06 EV).

### Segunda análise completa (pós-itens 1 e 2)

- **Suíte de regressão** `tools/testes.py` (antes eram comandos soltos): validação Vulkan com sincronização em
  5 presets × 6 cenas (30 combinações, inclusive Bistro), fornalha branca, atmosfera vs referência, cintilação e
  pixels inválidos. Resultado: **tudo passou** — fornalha 0,94–1,00; atmosfera 4,05%; brilhinhos 0,24%;
  nenhum pixel inválido.
- Erro encontrado e corrigido no próprio teste de pixels: `np.roll` dava a volta na imagem e comparava o céu
  noturno do topo com o chão da base (falso positivo de 52 pixels). Agora usa borda replicada.
- Revisão do código novo: amostrador do volume 3D com CLAMP nos três eixos (sem vazamento nas bordas da tela),
  barreiras entre escrita e leitura das LUTs confirmadas pela validação de sincronização, sem NaN para pontos
  abaixo do chão. Comentário trocado de lugar em `main.cpp` corrigido; `--atmosphere-test` incluído no `--help`.
- Modo janela (Xvfb) com validação: 60 s sem erros. Renders do pôr do sol (sol a 35°, 8° e 2°) conferidos.

### Item 3 recuperado — sombra do sol sem acne e sem degraus no terminator ✅

Medição antes de mexer (nova visualização `--debug-view sol|sol-luz|nl|normal`: valor cru, sem denoiser/TAA,
e cenas de teste `shadows` e `shadows-far` — a mesma cena a 4 km da origem). Métrica do `tools/sombras.py`:
**luz perdida** = Σ N·L × (1 − visibilidade) / Σ N·L nas esferas de 6 a 24 segmentos com normais suavizadas,
onde nada faz sombra (o ideal é 0%).

| | Sol a 20° | Sol a 3° | 4 km, sol a 20° | 4 km, sol a 3° | Acne no chão |
|---|---|---|---|---|---|
| Antes | até **44,7%** (degraus) | até 25,8% | até 42,5% | até 27,0% | 0% |
| Depois | **0,0%** | **0,0%** | 0,6% | 2,4% | 0% |

O que mudou (a mesma combinação do Blender Cycles):
- **Visibility buffer** no G-buffer (`R32G32_UINT`: objeto + triângulo). Os passes de RT recalculam a posição
  exata (raio da câmera ∩ plano do triângulo), a normal da faceta e a normal suave dos vértices — em vez de
  reconstruir pela profundidade (que errava ~1e-4 × distância e nas quinas).
- **Hanika 2021** ("Hacking the Shadow Terminator", Ray Tracing Gems II cap. 4): os raios de sombra saem do
  ponto projetado nos planos tangentes dos vértices, isto é, da superfície curva que o sombreamento descreve,
  e não da faceta plana que fica abaixo dela. Aplicado no pixel e nos pontos atingidos por raios (GI/reflexo).
- **Chiang 2019** ("Taming the Shadow Terminator"): termo de sombreamento do relevo para normal maps — a luz
  vai a zero de forma suave onde a superfície real já está de costas.
- Teste de "frente para a luz" com a normal suave (antes: faceta, que cortava a luz em degraus).
- Margem da origem dos raios: 1e-6 × distância com posição exata (era 1e-4: 1 cm a 100 m de folga = frestas de
  luz em sombras de contato); continua 1e-4 só onde o triângulo não é conhecido.

Verificado: nenhum vazamento de luz novo (comparação pixel a pixel fora das esferas: só bordas e ruído de
penumbra mudaram); Bistro (rua, com sol) e Sponza sem acne — as listras das cortinas do Sponza já existem no
N·L puro (moiré do normal map do tecido), não são sombra. Suíte completa passando, com o novo `teste_sombras`.

Limite conhecido: o resíduo de 2,4% a 4 km com sol rasante é a precisão do float32 nessa distância (0,5 mm por
passo). A solução definitiva é **renderização relativa à câmera** (origem flutuante, como UE5/Frostbite), que
entra no item de mundo aberto/streaming porque mexe em todos os shaders.

### Item 4 recuperado — ReSTIR de produção ✅

Medição primeiro (`tools/restir.py`, nova visualização `--debug-view luzes|luzes-ref`): a luz das luzes locais
crua (sem denoiser) contra uma referência por força bruta (todas as luzes no alcance, 16 amostras cada, com
sombra), nas cenas noturnas congeladas. **O ReSTIR antigo perdia luz: viés de −15% (Sponza) e −31% (externa).**

Causas encontradas, uma a uma, com experimentos isolados (`--restir-no-reuse temporal|spatial|all`):
1. O reservatório guardava só *qual* luz; o teste de visibilidade ia ao centro dela e o sombreamento sorteava
   outro ponto — luzes parcialmente visíveis eram descartadas inteiras.
2. Combinação temporal/espacial com pesos 1/M (versão enviesada de 2020): domínios que não geram uma amostra
   recebiam crédito por ela.
3. A visibilidade muda entre frames (jitter do TAA, objetos que se movem), então o histórico também recebia
   crédito por pontos que nunca teria gerado.

O que mudou:
- **Amostra = ponto na superfície da luz** (luz + coordenadas na esfera, medida de área). O mesmo ponto é
  testado, sombreado e reaproveitado; descartar amostras ocultas deixa de enviesar.
- **BVH de luzes** (Conty Estevez & Kulla 2018, como PBRT-v4/Cycles), reconstruída por frame na CPU: candidatos
  por importância (potência, distância, alcance, horizonte da normal) com pdf exata; escala para cidades.
  Também escolhe a luz nos pontos atingidos pela GI (antes: uniforme).
- **GRIS** (Lin et al. 2022): reuso temporal com balance heuristic avaliando as amostras na **superfície exata
  do frame anterior** (visibility buffer anterior + transformações e câmera anteriores) e reuso espacial com
  **MIS em pares** (Bitterli 2022 / Wyman et al. 2023), vizinhos com superfície exata.
- **Correção de viés por ray tracing** (presets Alto/Ultra/Cinema): visibilidade da amostra central vista de
  cada vizinho e da superfície anterior; amostra do histórico retestada no ponto atual.
- **Filtro de boiling** (como no RTXDI) e **reuso da visibilidade final** (amostra oculta no sombreamento não
  volta no próximo frame).
- Erro achado no caminho: conversão de pdf de ângulo sólido para área estava invertida (corrigida e conferida
  numericamente em Python); raio de visibilidade cortado a 0,999·d vazava luz de esferas encostadas em paredes.

| | Viés Sponza | Viés externa | Erro mediano/pixel Sponza | Erro mediano/pixel externa | Brilhinhos (teste) |
|---|---|---|---|---|---|
| Antes | −15,5% | −30,7% | 0,231 | 0,428 | 0,24% |
| Depois | **−0,5%** | **−1,4%** | **0,216** | **0,250** | **0,05%** |

Achado lateral (fica para a correção 3 — luzes reais): as esferas "lâmpada" das cenas têm o raio exato da luz,
e um raio que vai até a superfície da esfera às vezes bate na malha da lâmpada. Por isso todos os raios de
luz param 1e-5·d antes do ponto (a referência também).
