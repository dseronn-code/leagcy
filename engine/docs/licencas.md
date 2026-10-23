# Licenças de terceiros

O Legacy Engine baixa estas bibliotecas na compilação (CMake FetchContent). Nenhuma exige pagamento.

| Biblioteca | Uso | Licença | Obrigações |
|---|---|---|---|
| [NVIDIA NRD](https://github.com/NVIDIA-RTX/NRD) v4.18.0 | Denoiser ReLAX | NVIDIA RTX SDKs License | Ver abaixo |
| [NVIDIA NRI](https://github.com/NVIDIA-RTX/NRI) v190 | Camada que liga o NRD ao nosso Vulkan | MIT | Manter o aviso de copyright |
| [NVIDIA ShaderMake](https://github.com/NVIDIA-RTX/ShaderMake), [MathLib](https://github.com/NVIDIA-RTX/MathLib) | Ferramentas de build do NRD | MIT / NVIDIA | Só usadas na compilação |
| [DirectX Shader Compiler](https://github.com/microsoft/DirectXShaderCompiler) | Compila os shaders do NRD (só na compilação) | Licença LLVM (University of Illinois/NCSA) | Não é distribuído com o motor |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Memória de GPU | MIT | Manter o aviso |
| [GLM](https://github.com/g-truc/glm) | Matemática | MIT | Manter o aviso |
| [GLFW](https://github.com/glfw/glfw) | Janela e entrada | zlib | — |
| [cgltf](https://github.com/jkuhlmann/cgltf) | Leitor glTF | MIT | Manter o aviso |
| [stb_image](https://github.com/nothings/stb) | Imagens | MIT / domínio público | — |
| [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | Headers (dentro do NRI) | Apache 2.0 / MIT | — |

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
