# Pênalti da Treta — Android (Legacy Engine)

App nativo: o Legacy Engine (C++20 + Vulkan) desenhando direto na janela do Android, com o jogo de pênaltis de
`src/game/`. Mesmo código do desktop; o que muda é a entrada (`app/src/main/cpp/android_main.cpp`) e o build sem as
bibliotecas da NVIDIA (`LEGACY_NRD=OFF`: denoiser SVGF e TAAU do próprio motor).

## Estado

- **Ainda não foi compilado.** O ambiente onde foi escrito não tinha acesso ao SDK/NDK do Android (`dl.google.com`
  bloqueado). O motor e o jogo foram compilados e testados no Linux, inclusive na configuração de celular
  (`cmake -DLEGACY_NRD=OFF`, com e sem ray tracing); o código específico do Android (este diretório) não.
- **Nunca rodou num celular.** A primeira instalação é o primeiro teste. Se o motor não conseguir iniciar, o app mostra
  uma caixa de diálogo com o motivo (em vez de fechar sem explicação) — tire um print e mande.

## Celulares compatíveis

O motor exige Vulkan 1.3 com `VK_KHR_push_descriptor`, **geometry shader** (usado para identificar cada triângulo na
tela) e **inteiros de 64 bits em shader**:

- Snapdragon com GPU Adreno 6xx/7xx em Android 13+: provável que funcione.
- Exynos e Dimensity (GPUs Mali/Immortalis) e Tensor: a maioria **não** tem geometry shader — o app vai abrir e mostrar
  o erro. Suportar esses aparelhos exige trocar o "visibility buffer" do motor por outro caminho.
- Ray tracing (sombras e reflexos de verdade) liga sozinho nos que têm: Adreno 740/750+, Immortalis-G715+, Xclipse.
  Sem ray tracing a imagem perde as sombras das luzes do estádio.

## Compilar

Precisa do Android Studio (ou só do SDK + NDK 27.2.12479018 + CMake 3.31), Java 17 e internet (o CMake baixa as
bibliotecas do motor do GitHub):

```bash
cd engine/android
./gradlew assembleRelease        # Windows: gradlew.bat assembleRelease
```

O APK sai em `app/build/outputs/apk/release/app-release.apk` (assinado com a chave de debug: instala direto, fora da
loja). No celular: permitir "instalar apps desconhecidos" para o app que abrir o arquivo.

## Como funciona

- `prepareAssets` (Gradle): compila os shaders GLSL de `engine/shaders` em SPIR-V com o `glslc` do NDK e copia
  `engine/game` (personagem, texturas, fontes) para os assets do APK.
- Na primeira abertura, `android_main.cpp` copia esses arquivos para a pasta interna do app e aponta o motor para lá
  (`core::setDataDirs`).
- Preset de celular: resolução interna de 50% reconstruída pelo TAAU, 1 amostra por pixel, ReSTIR enxuto
  (`QualityPreset::Low`), retrato, 30/60 Hz com vsync.
- Toque = o `Pointer` do jogo (o mesmo do clique do mouse no desktop).
