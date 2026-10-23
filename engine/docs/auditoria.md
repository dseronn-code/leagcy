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

1. **Base numérica e física**: unidades fotométricas + pré-exposição + float32 onde precisa (1.1, 1.2, 2.5)
2. **Ruído e amostragem**: STBN, amostragem correta da esfera, offset robusto com normal geométrica (1.5, 1.6, 1.11)
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
