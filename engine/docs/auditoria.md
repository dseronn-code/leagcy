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
3. ✅ **Luzes reais** (29/10): luzes de malha + `KHR_lights_punctual` + globo da lâmpada sem sombra e sem contar duas
   vezes (1.3, 1.4, 2.2) — ver "Itens 3 e 4 do plano original" abaixo
4. 🟡 **Assets profissionais**: especular/glossiness (2.3), BC7/BC5 (2.10), DDS (2.1), vidro de parede fina (2.4),
   doubleSided (2.6), samplers/oclusão/câmera (2.8), cobertura alfa nos mips (2.9) ✅; faltam MikkTSpace de
   referência (2.7, aguardando aprovação para baixar a biblioteca), `KHR_texture_transform`, `COLOR_0`, `TEXCOORD_1`
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

### Item 5 — denoiser: NVIDIA NRD integrado, mas o SVGF próprio continua o padrão (medido)

O que entrou:
- **NVIDIA NRD v4.18.0** (ReLAX e ReBLUR) pela camada oficial NRDIntegration + NRI, embrulhando o nosso
  VkDevice (`--denoiser relax|reblur`). Normal em R10G10B10A2 (a codificação testada pela NVIDIA), viewZ e
  distância dos raios de GI/reflexo gravadas pela iluminação; ReBLUR recebe YCoCg + distância normalizada.
  Validação Vulkan com sincronização limpa (achou e corrigimos: a imagem de movimento precisava de uso STORAGE).
- **Demodulação do especular** (o NRD recomenda; vale também para o SVGF): o especular vai ao denoiser dividido
  pela refletância especular esperada (DFG) e é remultiplicado na composição — o detalhe das texturas de f0 não
  é mais borrado com o ruído. Fornalha idêntica (a conta é exata).
- `--nrd-validation`: camada de validação do NRD (histórico acumulado, desoclusão, vetores de movimento).
- `tools/denoiser.py`: câmera andando 24 frames contra a referência na mesma pose (`--move-stop`), média dos
  dois denoisers convergidos para não favorecer nenhum.

Medições (Sponza à noite, 640x360, Ultra):

| | Parado: cintilação | Parado: brilhinhos | Andando: erro | Andando: brilhinhos |
|---|---|---|---|---|
| SVGF próprio | 0,0216 | 0,02% | **0,112** | **0,44%** |
| NRD ReLAX | **0,0162** | **0,00%** | 0,139 | 0,75% |
| NRD ReBLUR | — | — | 0,173 | 1,29% |

Investigado antes de concluir: sinal dos vetores de movimento, sinal do jitter, convenção do eixo Y da projeção,
anti-lag, histórico rápido, filtro espacial mais forte, peso da distância dos raios. A camada de validação do
NRD confirma vetores corretos e histórico mantido em movimento. Em movimento o NRD deixa "confete" colorido (as
amostras de luzes coloridas do ReSTIR em pixels de histórico curto), que o SVGF (guiado por variância) espalha.
**Decisão pelos números:** SVGF continua o padrão (jogo = câmera em movimento); NRD fica disponível e é melhor
parado. Próximo passo para o NRD render mais: entrada de confiança do histórico (`IN_DIFF_CONFIDENCE`) vinda do
ReSTIR e testar no PC com RTX 3070 Ti (lá o NRD roda nos tempos para que foi ajustado).

Achado lateral: o metal de rugosidade 1 converge mais devagar com o NRD (fornalha 0,925 em 48 frames, 0,937 em
96); o teste de energia passou a usar 96 frames.

### Item 6 — TAA/upscaling: TAA mais nítido + DLSS e FSR 3 integrados (com volta automática ao TAAU)

O que entrou:
- **TAA próprio mais nítido** (medido em `tools/taa.py`, referência renderizada com 4x os pixels): a amostra atual
  é reconstruída por uma gaussiana estreita 3x3 (`exp(-8 d²)`, k escolhido por varredura) no lugar da leitura
  bilinear, e a **RCAS da AMD FidelityFX** (FSR 1, MIT) é aplicada na saída já tonemapeada, fora do histórico
  (`--sharpness`, padrão 0,5).
- **NVIDIA DLSS Super Resolution** (SDK oficial v310.9.1, inclui o modelo "transformer" do DLSS 4) e **AMD FSR 3**
  (FidelityFX SDK v1.1.4) pela extensão Upscaler do NVIDIA NRI (`src/renderer/upscaler.cpp`), escolhidos com
  `--upscaler dlss|fsr`. `--scale` escolhe o modo: 1 = DLAA/nativo, 0,67 = qualidade, 0,58 = equilibrado,
  0,5 = desempenho. O fabricante define a resolução interna, o número de fases do jitter (Halton com 8·fator²
  fases, ex.: 32 no desempenho) e o **mip bias** das texturas (ex.: −2 no desempenho), que agora o G-buffer aplica.
- **Volta automática**: se a GPU não suporta (DLSS só em RTX; FSR só no build do Windows) o motor avisa e usa o
  TAAU próprio — testado aqui: imagem idêntica bit a bit ao TAAU.
- Dispositivo Vulkan: liga todas as features que a GPU suporta nas estruturas core 1.1/1.2/1.3 (menos robustez,
  que custa desempenho), como o NRI faz no device que ele mesmo cria. **Achado do teste:** o NRI, o DLSS, o FSR e o
  NIS consultam o que a GPU suporta e assumem que está ligado; sem isso a validação acusou 4 erros (timeline
  semaphore, float16, descritores "update after bind") que apareceriam também na RTX 3070 Ti. Extensões
  `VK_NVX_binary_import` e `VK_NVX_image_view_handle` (exigidas pelo DLSS) ligadas quando existem.

Convenções passadas aos upscalers (conferidas no guia do DLSS e no código do NRI):
- jitter = **+deslocamento da imagem em pixels de render** (nossa projeção desloca a imagem em +jitter, x para a
  direita e y para baixo, o mesmo sistema dos vetores de movimento); o NRD recebe o sinal oposto (convenção dele);
- vetores de movimento: guardamos (atual − anterior) em UV, sem jitter → escala `−resolução` (o DLSS quer anterior
  − atual em pixels);
- profundidade com Z invertido e plano distante no infinito; cor HDR já pré-exposta (textura de exposição = 1);
- entradas passam a `SHADER_READ_ONLY_OPTIMAL` só durante o despacho (o guia do DLSS exige estado de leitura) e
  voltam a `GENERAL`; a saída fica em `GENERAL` (storage).

Verificação aqui (sem GPU NVIDIA/AMD): compila com o SDK do DLSS embutido; `--upscaler dlss|fsr` cai no TAAU sem
erro; o **NIS** da NVIDIA (upscaler espacial que roda em qualquer GPU, `--upscaler nis`, só para teste) percorre
o mesmo caminho do módulo — embrulhar device e imagens, command buffer, transições, saída, mip bias — e passa na
validação Vulkan com sincronização sem nenhum erro. Imagem padrão idêntica bit a bit à do commit anterior.

Falta conferir na RTX 3070 Ti (não dá para rodar DLSS aqui): imagem estável com a câmera parada (jitter com o
sinal certo — se tremer ou ficar "mole", o sinal do jitter está invertido) e sem rastros em movimento (vetores).
Limitação conhecida: a pré-exposição muda devagar com a auto-exposição e o upscaler recebe `preExposure = 1`; em
mudanças bruscas de luz o histórico do DLSS/FSR leva alguns frames a acompanhar (o valor real só existe na GPU).

**Medido na RTX 3070 Ti** (`tools/teste_pc.py`, 1920x1080 Ultra; nitidez e erro contra referência 4x a 640x360):

| | Nitidez | Erro | Tempo do passe |
|---|---|---|---|
| TAA do motor, nativo | 0,894 | 0,0297 | 0,16 ms |
| DLAA (DLSS nativo) + RCAS 0,5 | 1,098 | **0,0257** | 1,17 ms |
| TAAU 67% | 0,722 | 0,0505 | 0,14 ms |
| DLSS qualidade (67%) + RCAS 0,5 | 1,049 | **0,0404** | 1,16 ms |
| FSR 3 qualidade (67%) + RCAS 0,5 | 0,979 | 0,0597 | 0,60 ms |
| DLSS desempenho (50%) + RCAS 0,5 | 1,037 | 0,0586 | 2,09 ms |

O jitter e os vetores de movimento estão certos (o DLSS erra menos que tudo; com sinal trocado ele sairia mole).
Decisões: **DLSS virou o padrão** em GPUs RTX (`--upscaler auto`; sem RTX, TAAU sem aviso); com DLSS a RCAS fica
desligada por padrão (1,05–1,10 = nítido demais com ela). O FSR 3 erra mais que o nosso TAAU nesta cena; fica
como opção para placas AMD. Frame inteiro em 1080p nativo: 68 ms, dominado por ReSTIR (31 ms) e iluminação RT
(29 ms) — otimização fica para o fim, depois da qualidade (decisão do usuário).

### Achados na RTX 3070 Ti (segunda bateria do `tools/teste_pc.py`)

- **Precisão longe da origem (mundo aberto) — corrigido na raiz.** O teste de sombra a 4 km acusou 3,0% (sol a 20°) e
  9,4% (sol a 3°) de luz perdida no contorno das esferas (2,4% no lavapipe). Investigado em ordem: deslocamento da
  origem do raio de sombra escalado pela maior coordenada (sem efeito), ponto reconstruído pelas baricêntricas
  (sem efeito), e então a causa: a direção do raio de cada pixel vinha de `invViewProj` absoluta, aplicada a um ponto
  a 20 cm da câmera com coordenadas de 4 km — erro de ulp(4000) ≈ 0,2 mm nesse ponto = direção torta em ~1e-3 rad
  (3 cm a 30 m). Correção: **renderização relativa à câmera** (como os motores de mundo aberto fazem): a inversa
  da projeção não tem mais translação (`cameraRelativePosFromDepth` + câmera somada no fim) e o vertex shader do
  G-buffer projeta `posição - câmera` (diferença exata de valores próximos). Resultado: **0,0% a 4 km** nos dois
  ângulos de sol, igual à origem; o limite do teste a 4 km caiu de 5% para 1%.
- **Validação com DLSS:** o NGX limpa algumas imagens entregues a ele com `vkCmdClearColorImage`; as imagens de cor,
  profundidade, movimento e saída agora têm `TRANSFER_DST`.
- Com DLSS (padrão) a cintilação parada caiu para 0,0145 e brilhinhos 0,00% (TAA do motor no lavapipe: 0,0245 / 0,04%).

### Item 5 (denoiser) — DLSS Ray Reconstruction integrado

O denoiser de ponta hoje em placas RTX é o **DLSS Ray Reconstruction** (Cyberpunk 2077 com path tracing, Alan
Wake 2, Indiana Jones): uma rede neural que faz o denoiser e o upscaler juntos, com o histórico em resolução de saída.
Integrado pelo mesmo caminho do DLSS (`--denoiser dlss-rr`, tipo DLRR do NRI), seguindo o guia oficial de integração:
- a iluminação vai **ruidosa** para a composição (sem SVGF/NRD) e a cor composta entra no DLSS-RR;
- imagens-guia na resolução de render (`shaders/rr_guides.comp`), consistentes com a composição: albedo difuso,
  refletância especular média (DFG × compensação de energia — a mesma que remodula o especular), normal de shading
  no mundo + rugosidade linear (modo "packed"), distância do raio especular; céu com os valores padrão da NVIDIA
  (albedo 0,5, normal 0, distância 65504);
- matrizes mundo→câmera e câmera→clip sem jitter para o DLSS-RR calcular o movimento dos reflexos; 32 fases de
  jitter (o guia pede "pelo menos 32"); nitidez RCAS desligada como no DLSS;
- sem RTX, volta ao SVGF + TAAU do motor. Passe de guias validado aqui com a validação Vulkan (forçado).

`tools/denoiser.py` agora mede cada denoiser andando (erro contra referência convergida) e parado (cintilação e
brilhinhos); o `tools/teste_pc.py` compara SVGF, NRD ReLAX e DLSS-RR na RTX. O padrão só muda com a medição.

### Item 7 — reflexos e luz indireta: cache de radiância, reflexo do reflexo, movimento virtual (medido contra path tracing)

O que entrou:
- **Referência por path tracing** (`--reference-pt`): sem denoiser, rebotes por path tracing (até 8, roleta russa)
  acumulados com a câmera parada — a verdade para GI e reflexos. Usa o mesmo limite de vagalumes do motor por
  amostra, para medir reflexos e luz indireta e não cáusticas (sol refletido por espelho), que motor de jogo nenhum
  calcula. Cena de teste `reflections`: sala com piso de cromo, paredes coloridas, esferas de ouro de rugosidade
  0–0,6, sol só na parede do fundo. `tools/reflexos.py` compara.
- **Cache de radiância no espaço do mundo** (`shaders/radiance_cache.glsl`, no espírito do SHaRC da NVIDIA): tabela
  hash de células (posição quantizada, tamanho dobrando com a distância à câmera, + direção da normal), guardando a
  radiância incidente média sem sol nem luzes locais. Alimentado por 1/4 dos pixels na superfície visível e um rebote
  além; resolvido por frame (média de 32 frames, em nits). Substitui o "céu sem oclusão" nos pontos atingidos por
  raios (auditoria 3.1): infinitos rebotes nos reflexos e na GI. `--no-radiance-cache` desliga.
- Pontos atingidos espelhados (rugosidade < 0,3) refletem por mais um raio: o espelho mostra o reflexo do reflexo.
- **Movimento virtual do reflexo no SVGF** (auditoria 1.13): o especular é reprojetado pelo ponto virtual atrás do
  espelho (distância do reflexo suavizada em 3x3), misturado ao da superfície pela rugosidade, como no NRD.
  `--no-virtual-motion` desliga (medição).

Medido (sala de reflexos, 480x270, referência 256 frames, motor 64 frames):

| | Erro | Viés | Erro no espelho | Erro nas esferas |
|---|---|---|---|---|
| Sem cache (antes) | 0,664 | −64,2% | 0,645 | 0,478 |
| Com cache | **0,068** | **−4,3%** | **0,156** | **0,086** |

(Sem cache medido contra a referência anterior, com cáusticas; com elas, o cache dava 0,117 / −10%: o resto do viés
eram as cáusticas.) Validação Vulkan com sincronização limpa no cache e na referência.

### Fase 2, item 1 — texturas comprimidas (BC7/BC5)

- `src/scene/texture_compress.cpp` com o bc7enc_rdo (Rich Geldreich, MIT/domínio público, via git): mipmaps na CPU
  (cor em espaço linear; dados por média direta; normais renormalizadas), **BC7** com pesos perceptuais na cor e
  **BC5** nos normal maps (x e y; z reconstruído no shader), várias threads, cache em disco (`texcache/`: a Sponza
  comprime em ~20 s na primeira carga e carrega em ~1 s depois). `--no-texture-compression` volta ao RGBA8.
- Por que BC5 nos normais (medido): em BC7 o pior normal map da Sponza ficava com 26,4 dB (mais qualidade no
  codificador não mudou: o BC7 divide os bits entre três canais); em BC5, 34,9 dB.

Medido (Sponza):

| | RGBA8 | BC7/BC5 |
|---|---|---|
| Memória de vídeo das texturas (com mips) | 459 MB | **115 MB (4,0x menos)** |
| Qualidade das texturas (PSNR médio / pior) | — | 46,8 dB / 34,9 dB |
| Imagem final contra RGBA8 (diferença / viés) | — | 0,016 / +0,4% |
| Cintilação parado (suíte) | 0,0245 | 0,0252 |

Troca conhecida: o BC5 guarda a normal unitária, então o Toksvig (variância das normais dentro do pixel, lida no
encurtamento da normal média dos mips) deixa de existir nos normal maps; o termo por derivadas de tela do AA
especular continua. Efeito medido: cintilação +3% (0,0245 → 0,0252), brilhinhos iguais (0,04%). Melhoria possível
depois: guardar a variância das normais na rugosidade dos mips (como o Unreal faz).

### Terceira bateria na RTX 3070 Ti (28/10) — item 5 fechado, e um achado no Bistro

Suíte 7/7 na RTX. Denoisers contra a **referência verdadeira** (acumulação sem denoiser, `tools/denoiser.py`),
Sponza à noite:

| | erro andando | brilhinhos andando | erro parado | cintilação parado |
|---|---|---|---|---|
| SVGF (cena congelada) | 0,163 | 1,17% | 0,129 | 0,0131 |
| NRD ReLAX | 0,211 | 0,75% | 0,203 | 0,0135 |
| **DLSS-RR** | **0,108** | **0,12%** | **0,105** | **0,0058** |
| SVGF (personagens e luzes animados) | 0,170 | 1,19% | 0,137 | 0,0182 |
| NRD ReLAX | 0,213 | 0,75% | 0,211 | 0,0184 |
| **DLSS-RR** | **0,112** | **0,14%** | **0,115** | **0,0120** |

Sala de espelhos andando: DLSS-RR erro 0,105 e 0 % de brilhinhos (SVGF 0,149 / 0,29 %, ReLAX 0,111 / 0,40 %); parado
o ReLAX cintila um pouco menos (0,0072 contra 0,0087), com erro maior (0,075 contra 0,071). Custo: +2 ms sobre o
DLSS comum (47,8 contra 45,6 ms/frame no Ultra a 1080p; o SVGF que ele substitui custa 2,7 ms).

**Decisão (item 5 ✅):** numa RTX o padrão (`--upscaler auto`) passa a ser o **DLSS Ray Reconstruction**, o mesmo
denoiser neural do Cyberpunk 2077 com path tracing; `--denoiser svgf|relax|reblur` continua escolhendo o do motor.
Sem RTX o padrão continua SVGF + TAAU.

**Achado — o Bistro não tinha texturas.** O pacote só traz as imagens em DDS/BC7 (`MSFT_texture_dds`) e o leitor só
abria PNG/JPG: todas as medições anteriores no Bistro foram sem textura (o problema 2.1 desta auditoria nunca tinha
sido corrigido). Corrigido: `loadDds` lê os blocos BC7 e os mips como estão e envia sem recompressão (338 texturas,
2,2 GB de memória de vídeo; em RGBA8 seriam ~9 GB). Sem BC7 na GPU, o nível 0 é decodificado e vai em RGBA8.
Revisando o plano acima também apareceu que os itens 3 (luzes reais) e 4 (assets) do plano original nunca foram
feitos — agora estão marcados e entram na fila antes da otimização.

### Itens 3 e 4 do plano original (29/10) — luzes reais e assets profissionais

**Luzes de malha.** Cada triângulo de um objeto emissivo vira uma luz da BVH de luzes, amostrada pelo ReSTIR e pela
luz direta nos pontos atingidos por raios (como o RTXDI faz no Bistro): potência por triângulo estimada na textura
emissiva, radiância exata no shader (textura nas UVs do ponto), um lado só (ou os dois com `doubleSided`); objetos que
se movem levam as luzes junto. A emissão de luzes amostradas não soma nos pontos atingidos por raios (somaria duas
vezes, 1.4), exceto no raio de transmissão do vidro (o ReSTIR só avalia reflexão). Globos que são a forma visível de
uma luz analítica não fazem sombra (1.3).

Prova de energia por path tracing sem limite de vagalumes: a mesma cena COM luzes de malha × SEM (a luz dos
emissivos só pelos raios que batem neles — o caminho ingênuo, sem viés). Dois erros achados e corrigidos no caminho:

| Cena | 1ª medição | Causa | Depois |
|---|---|---|---|
| Globo emissivo (992 triângulos) | −51% | o raio de sombra esbarrava no próprio triângulo da luz (folga de 0,001% no fim do raio; agora 0,1%) | **−0,3%** |
| Sala com painel, globos e capacete | −16% nas paredes | a janela de alcance das luzes pontuais (corte artístico) tirava luz bem antes do corte | sem janela nas luzes de malha |
| Painel no ar | — | — | **+0,02%** |

(Um painel encostado no teto, com a face de cima no mesmo plano do teto, dá −2%: geometria degenerada, nenhum dos dois
caminhos define o que acontece ali.) Também: a referência (`--reference-pt`) deixou de usar o filtro de boiling do
ReSTIR, que descarta amostras de propósito; e os raios especulares passaram a pesar G2/G1 correlacionado (antes G1,
o G2 não correlacionado), o mesmo BRDF que a amostragem de luzes avalia.

Bistro à noite: 76 mil triângulos emissivos (letreiros, lâmpadas, cordões de luz) + as 96 luzes do arquivo. Antes, a
luz dos emissivos só chegava por raios de GI — névoa roxa e pontinhos; agora a rua é iluminada pelos postes e pelos
cordões.

**Assets.**
- DDS/BC7 (`MSFT_texture_dds`) sem recompressão; câmera do glTF como vista inicial; samplers por textura;
  `doubleSided` com culling (lotes com cull/frente dinâmicos, imagem idêntica na cena de reflexos);
  `occlusionTexture` só na luz indireta; mips de recorte por alfa com a cobertura preservada (Castaño).
- **Vidro de parede fina** (`KHR_materials_transmission`): o pixel de vidro traça o lobo especular espelhado para
  trás da superfície (peso (1 − F) × cor × transmissão); raios de GI/reflexo atravessam até 4 lâminas tingindo; vidro
  não faz sombra (sombra colorida e refração com volume ficam para depois).
- **Pintura de carro**: verniz `KHR_materials_clearcoat` (GGX dielétrico na normal suave, com sol, luz do ReSTIR e
  raio de reflexo próprios; a base recebe 1 − cc × F) e flocos metálicos (de perto cada floco brilha; de longe viram
  rugosidade, sem cintilar).
- Céu noturno: lua cheia como luz direcional (0,25 lux, disco com mares; o céu de lua sai azul como em fotos de longa
  exposição) e estrelas (magnitudes −1 a 6, apagadas no horizonte pela atmosfera).
- Pendente: MikkTSpace de referência (o download da biblioteca foi bloqueado pela política de permissões do ambiente;
  a versão pronta está guardada para quando for aprovada).

### Noite de 29 para 30/10 — da tabela do GTA 6: 11 ✅ / 4 🟡 / 16 ❌ → 23 ✅ / 9 🟡 / 0 ❌

Tudo verificado aqui (llvmpipe, sem GPU de verdade): camadas de validação do Vulkan limpas em cada recurso novo e
suíte 7/7 no binário com todos eles até o streaming. Os números de desempenho e a aparência final dependem da
bateria na RTX (`python tools\teste_pc.py`, com a nova seção "galeria").

| Recurso | O que foi feito | Como foi conferido |
|---|---|---|
| Vidro | parede fina e com volume (Snell, reflexão interna total, Beer), sombra colorida, BLEND como decal no G-buffer | cena `glass`: imagem invertida na bola maciça, sombra azul do painel |
| Pintura de carro | verniz (clearcoat) + flocos metálicos | cena `paint` |
| Nuvens e raios de luz | nuvens Nubis/Hillaire no céu, reflexos e sombra no chão; névoa volumétrica em froxels com sombra traçada | cenas externas, `--fog 0.02` na Sponza |
| Chuva | molhado, poças com reflexo e anéis, pingos, respingos, abrigo sob telhados | cidade `--rain 0.8` |
| Oceano | FFT de Tessendorf (IFFT conferida contra numpy: erro 2e-14), espuma, crista translúcida | cena `ocean` |
| Lens flare | fantasmas, halo, dispersão cromática, sujeira na lente | diferença com/sem `--no-lens-flare` |
| Partículas | fogo, fumaça, faíscas, explosões, chuva, respingos (sem estado, na GPU) | cena `vfx`; achado: estouro do float16 à noite, corrigido |
| Vegetação | vento na folhagem (com vetores de movimento) e grama instanciada | Sponza: diferença só nas plantas |
| Pele e olhos | SSS separável, transmissão em partes finas, olho procedural | cenas `skin`, `skin-backlit` |
| Cabelo | fios com fita virada para a câmera, transparência estocástica, Kajiya-Kay/Scheuermann | cena `hair`; achado: sorteio igual para todos os fios, corrigido |
| Tecido | Verlet/PBD na GPU | bandeira na cena externa |
| Multidão | animação assada por pose, BLAS da pose | cena `crowd`: 300 pessoas, 1,6 ms de CPU |
| Sombra sem RT | cascatas 2048² com PCF | Sponza `--shadow-maps` ≈ sombra traçada |
| GPU-driven | culling por frustum em compute + desenho indireto | imagem idêntica com/sem |
| LOD | simplificação por erro quádrico, troca por distância no culling | Bistro: 0,3% dos pixels; achado: dois bugs de índice, corrigidos |
| Streaming | células de malhas + BLAS, TLAS compacto | cena `stream`: 1.600 torres únicas, ~126 residentes; achados: travamento do llvmpipe com instâncias inativas e objetos que não reapareciam, corrigidos |
| Assets | KHR_texture_transform, decals procedurais | — |

Pendências conhecidas: MikkTSpace de referência (o download da biblioteca foi barrado pela política de permissões do
ambiente — precisa de aprovação), COLOR_0, HLOD, terreno por clipmaps, carregamento assíncrono do
disco, streaming de texturas, Marschner completo e física dos fios, vista subaquática.

### 30–31/10 — oclusão HiZ em duas fases (e dois bugs que ela revelou)

Fase 0 desenha o que estava visível no frame anterior; uma pirâmide de profundidade (HiZ, a mais distante por região)
é montada dessa passada; a fase 1 testa todos contra ela, desenha os que apareceram e guarda a visibilidade. Conferido
pela imagem: diferença **0** com e sem (`--no-occlusion`) na Sponza parada (com e sem LOD) e na cidade andando,
validação Vulkan limpa. Antes de chegar ao 0 o A/B mostrou vasos com plantas da Sponza piscando, por duas causas:

- **Folga do vento na unidade errada** (bug que já afetava o culling por frustum): a folga de 0,6 era somada à caixa
  em unidades da malha; o Sponza tem escala 0,008, então valia 5 mm, e o vento empurra a folhagem até ~1,1 m. Agora a
  folga é em metros do mundo, convertida pela menor escala do objeto no próprio shader.
- **`textureSize(hiZ, nível)` com nível diferente entre invocações vizinhas** devolvia, no llvmpipe, o tamanho de
  outro nível (achado isolando: com qualquer nível fixo 0–8 o erro sumia; com o nível calculado, voltava). O tamanho
  agora sai da regra do Vulkan (max(1, tamanho >> nível)), idêntica pela especificação e sem depender do driver.

Ganho no llvmpipe: −10% no G-buffer da cidade (aqui o custo é quase todo de pixel; o ganho real se mede na RTX).

### Meshlets por mesh shader (31/10)

Malhas rígidas com ≥ 8 meshlets (~1.000 triângulos) são desenhadas por `VK_EXT_mesh_shader` (llvmpipe e RTX 3070 Ti
têm): cada meshlet tem até 64 vértices e 124 triângulos, num trecho contínuo da lista de índices (o `gl_PrimitiveID`
continua o do triângulo original, então visibility buffer, luzes de malha e ray tracing não mudam). O task shader
testa 32 meshlets por instância visível (fora dos 4 planos laterais com 1 pixel de folga pelo jitter; todos os
triângulos de costas pelo cone das normais, só com escala uniforme) e mantém a ordem original dos que sobram. Vento,
cabelo, mar, multidão, esqueleto, tecido e decals continuam no desenho indexado. `--no-meshlets` desliga.

- **A/B** (meshlets x indexado, mesma configuração): Sponza parada, cidade andando, streaming, mundo de 8 km e
  Bistro — diferença média ≤ 0,15 nível de 255, e só 2 pixels acima de 20 (pontos das lâmpadas no Bistro); nenhum
  objeto ou triângulo falta. A sobra é arredondamento: o llvmpipe compila as mesmas contas de outro jeito no estágio
  mesh. Validação Vulkan limpa (Sponza, cidade, mundo).
- **Bug do llvmpipe (Mesa 25.2)**: um desenho indexado logo depois de um por mesh shader, na mesma passada de
  renderização, derruba o processo (achado isolando: até um `vkCmdDrawMeshTasksEXT(0, 1, 1)` com shaders vazios
  derrubava; só meshlets não). Os lotes por meshlets agora vêm depois de todos os indexados (também é uma troca de
  pipeline a menos por passada).
- **Desempenho**: no llvmpipe (CPU) a Sponza ganhou, e malhas pequenas repetidas perderam — daí o mínimo de 8
  meshlets por malha. O número que vale é o da RTX: `python tools\teste_pc.py --so gpudriven` (tudo ligado, sem
  oclusão, sem meshlets, sem culling, em 5 cenas, com a diferença de imagem de cada um).

### Cor por vértice — `COLOR_0` (31/10)

O glTF pode trazer uma cor por vértice que multiplica a cor base e o alfa. Agora ela é lida (vec3/vec4, float ou
inteiro normalizado), vai num buffer separado por malha (o vértice continua com 48 bytes; o endereço fica no campo
livre da instância, 0 = sem cor) e chega a todos os caminhos: G-buffer (vertex e mesh shader), reconstrução exata do
pixel pelo visibility buffer, raios de reflexo/GI/path tracing, recorte por alfa nos raios e nas shadow maps, e LOD
(média por grupo). Teste: o modelo oficial **VertexColorTest** da Khronos (agora em `tools/fetch_assets.cmake`) —
antes os três quadrados de cima mostravam "X"; agora mostram o mesmo "✓" da linha de referência (média de cor por
quadrado igual à de baixo com 1–2 níveis de diferença, no motor e no path tracing de referência). Cenas sem cor por
vértice: diferença máxima de 1 nível contra o build anterior.

### HLOD e troca suave de nível (31/10)

- **HLOD**: a cena marca o que nunca se move (`hlodCandidate`; a cidade marca tudo). No upload, os candidatos são
  agrupados por célula de 128 m e material; cada grupo com 2+ objetos vira um proxy fundido no espaço do mundo (com
  cor por vértice e espelhados tratados), simplificado para 1/4 quando passa de 2 mil triângulos. O culling da GPU
  troca membros por proxy a 250 m da caixa da célula. Proxies ficam fora do TLAS e das shadow maps (luz, reflexos e
  sombras sempre dos objetos reais) e são criados depois das luzes de malha. `--no-hlod` desliga.
- **Troca suave**: numa faixa de 10% em volta de cada distância de troca (LOD 0→1→2 e membros→proxy) os dois níveis
  desenham, com pontilhados complementares (o mesmo número aleatório por pixel decide quem fica, então nunca há
  buraco nem pixel duplo), sorteados de novo a cada frame. A entrada da lista visível passou a guardar o objeto em
  24 bits (máx. 16,7 milhões, com erro claro acima disso), 6 bits do pontilhado e 2 do nível.
- **Medido**: cidade 30×30 vista de cima, HLOD ligado x desligado: diferença média 0,04 (máx. 12) com centenas de
  proxies na tela. Câmera andando 24 quadros: só nos primeiros quadros (histórico curto do TAA) até 16 pixels acima
  de 20 pelo ruído do pontilhado; depois máx. 10–15, média ≤ 0,05 — nenhum estalo. Oclusão continua diferença 0;
  validação Vulkan limpa.

