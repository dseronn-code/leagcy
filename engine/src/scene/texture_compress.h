#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace scene {

// Textura comprimida em BC7 (1 byte por texel: 4x menos memória de vídeo que RGBA8), com todos os mips.
struct CompressedTexture {
  VkFormat format = VK_FORMAT_UNDEFINED;  // BC7 sRGB (cor) ou BC7 UNORM (dados lineares e normal maps)
  uint32_t width = 0, height = 0;
  std::vector<std::vector<uint8_t>> mips;  // blocos 4x4 de 16 bytes, nível 0 primeiro
};

// Mipmaps na CPU (cor: média em espaço linear; dados e normais: média direta — a normal média encurtada é a
// variância que o Toksvig do material usa) + BC7 de alta qualidade (bc7enc, Rich Geldreich: modos 1/5/6/7, pesos
// perceptuais na cor). Em várias threads; o resultado vai para um cache em disco (`cacheDir`), então só a primeira
// carga de cada textura comprime.
CompressedTexture compressTexture(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb, const std::string& cacheDir);

// PSNR (dB) do nível 0 decodificado contra o original, em RGBA (medição de qualidade).
double compressionPsnr(const CompressedTexture& t, const uint8_t* rgba);

}  // namespace scene
