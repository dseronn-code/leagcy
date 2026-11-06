# Licenças de terceiros

O Legacy Engine baixa estas bibliotecas na compilação (CMake FetchContent). Nenhuma exige pagamento.

| Biblioteca | Uso | Licença | Obrigações |
|---|---|---|---|
| [NVIDIA NRD](https://github.com/NVIDIA-RTX/NRD) v4.18.0 | Denoiser ReLAX | NVIDIA RTX SDKs License | Ver abaixo |
| [NVIDIA NRI](https://github.com/NVIDIA-RTX/NRI) v190 | Camada que liga o NRD e os upscalers ao nosso Vulkan | MIT | Manter o aviso de copyright |
| [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) v310.9.1 (opção `LEGACY_DLSS`) | DLSS e DLSS Ray Reconstruction (`--upscaler dlss`, `--denoiser dlss-rr`) | NVIDIA RTX SDKs License | Ver abaixo |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v1.1.4 (opção `LEGACY_FSR`, Windows) | FSR 3 (`--upscaler fsr`) | MIT | Manter o aviso |
| AMD FidelityFX RCAS (FSR 1) | Nitidez da saída (`shaders/tonemap.frag`) | MIT | Manter o aviso (no shader) |
| [NVIDIA Image Scaling](https://github.com/NVIDIAGameWorks/NVIDIAImageScaling) (dentro do NRI) | Só teste da integração (`--upscaler nis`) | MIT | Manter o aviso |
| [NVIDIA ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake), [MathLib](https://github.com/NVIDIA-RTX/MathLib) | Ferramentas de build do NRD | MIT / NVIDIA | Só usadas na compilação |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Compila os shaders do NRD (só na compilação) | Licença LLVM (University of Illinois/NCSA) | Não é distribuído com o motor |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Memória de GPU | MIT | Manter o aviso |
| [GLM](https://github.com/g-truc/glm) | Matemática | MIT | Manter o aviso |
| [GLFW](https://github.com/glfw/glfw) | Janela e entrada | zlib | — |
| [cgltf](https://github.com/jkuhlmann/cgltf) | Leitor glTF | MIT | Manter o aviso |
| [stb_image](https://github.com/nothings/stb) | Imagens | MIT / domínio público | — |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Headers (dentro do NRI) | Apache 2.0 / MIT | — |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) v5.2.0 | Física: corpos rígidos, personagem, colisão por malha | MIT | Manter o aviso |
| [EnTT](https://github.com/skypjack/entt) v3.13.2 | Entidades e componentes da camada de jogo | MIT | Manter o aviso |
| [nlohmann/json](https://github.com/nlohmann/json) v3.11.3 | Cenas em arquivo (.json) | MIT | Manter o aviso |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.92.8 (docking) | Interface do editor | MIT | Manter o aviso |
| [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo) | Alças de mover/girar no editor | MIT | Manter o aviso |
| [Lua](https://github.com/lua/lua) v5.4.7 | Scripts | MIT | Manter o aviso |
| [sol2](https://github.com/ThePhD/sol2) v3.5.0 | Ponte C++ ↔ Lua | MIT | Manter o aviso |
| [miniaudio](https://github.com/mackron/miniaudio) 0.11.24 | Áudio 3D | Domínio público ou MIT-0 | — |
| [Recast/Detour](https://github.com/recastnavigation/recastnavigation) 1.6.0 | Navegação dos NPCs | zlib | Não apresentar como próprio; manter o aviso nos fontes |
| [Roboto](https://fonts.google.com/specimen/Roboto) (fonte, `fontes/Roboto-Medium.ttf`) | Texto do HUD | Apache 2.0 | Manter o aviso de licença |
| Raposa (`assets/Fox`, glTF Sample Models) | Personagem animado de exemplo (`cenas/parque.json`) | CC-BY 4.0 | Créditos: PixelMannen, @tomkranis, AsoboStudio e @scurest |
| [ENet](https://github.com/lsalzman/enet) 1.3.18 | Multijogador (UDP) | MIT | Manter o aviso de copyright |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) 0.22 | Malhas comprimidas dos glTF | MIT | Manter o aviso de copyright |
| [Basis Universal](https://github.com/BinomialLLC/basis_universal) 1.60 (só o transcodificador, com zstd) | Texturas KTX2 dos glTF | Apache 2.0 (zstd: BSD) | Incluir o aviso da Apache 2.0 e o NOTICE ao distribuir |

## NVIDIA NRD — o que a licença exige (texto completo em `build/_deps/nrd-src/LICENSE.txt`)

- **Uso comercial é permitido**, inclusive em jogos vendidos, sem pagamento.
- **Crédito obrigatório:** jogos e aplicativos que usam o NRD precisam mostrar a marca da NVIDIA / o nome
  do SDK na **tela de créditos** (ou na documentação, se não houver tela de créditos).
- O aplicativo precisa ter funcionalidade própria relevante além do SDK (um motor gráfico inteiro tem).
- **Não é permitido** vender ou distribuir o NRD sozinho, nem colocá-lo sob licença de código aberto.
- Se o código-fonte do NRD for modificado e distribuído, incluir o aviso: "This software contains source
  code provided by NVIDIA Corporation." (hoje não modificamos o NRD).
- Se um dia o **motor** for vendido a outros estúdios, cada cliente recebe o NRD sob termos tão protetores
  quanto a licença da NVIDIA — na prática, os clientes também aceitam a licença do NRD. O SVGF próprio
  (`--denoiser svgf`) continua no motor como alternativa sem licença de terceiros.
- Não usar em sistemas críticos de segurança (aviação, medicina, uso militar).

## NVIDIA DLSS — o que a licença exige (texto completo em `build/_deps/ngx-src/LICENSE.txt`)

Mesma licença do NRD ("NVIDIA RTX SDKs"), com duas obrigações a mais, específicas do DLSS:

- **Logo/marca da NVIDIA na tela de abertura (splash screen)**, na janela "Sobre" (se existir) e nos créditos
  de jogos, informando que o produto usa o DLSS.
- **Avisar a NVIDIA antes do lançamento comercial** de qualquer produto com DLSS, pelo formulário
  https://developer.nvidia.com/sw-notification (empresa, desenvolvedora, SDK usado, nome do produto, plataforma,
  data prevista e link). Material de marketing com as marcas "NVIDIA RTX" precisa de aprovação prévia da NVIDIA
  (enviar amostra 2 semanas antes).
- As bibliotecas do DLSS (`nvngx_dlss.dll` / `libnvidia-ngx-dlss.so`) podem ser distribuídas junto com o jogo.
- Para um build sem DLSS (sem essas obrigações): `cmake -DLEGACY_DLSS=OFF` — o motor usa o TAAU próprio ou o FSR.
