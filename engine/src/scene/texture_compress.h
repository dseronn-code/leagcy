#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace scene {

// Textura comprimida (1 byte por texel: 4x menos memória de vídeo que RGBA8), com todos os mips.
struct CompressedTexture {
  VkFormat format = VK_FORMAT_UNDEFINED;  // BC7 sRGB (cor), BC7 UNORM (dados lineares) ou BC5 (normal maps)
  uint32_t width = 0, height = 0;
  std::vector<std::vector<uint8_t>> mips;  // blocos 4x4 de 16 bytes, nível 0 primeiro
};

// Mipmaps na CPU (cor: média em espaço linear; dados: média direta; normais: média renormalizada) + compressão de
// alta qualidade (bc7enc/rgbcx, Rich Geldreich): BC7 com pesos perceptuais na cor; normal maps em BC5 — dois canais
// independentes (x, y), o formato padrão da indústria para normais: o BC7 divide os bits entre três canais e erra
// muito em normais com detalhe fino (medido: 26 dB no pior normal map da Sponza). Em várias threads; o resultado vai
// para um cache em disco (`cacheDir`), então só a primeira carga de cada textura comprime.
CompressedTexture compressTexture(const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb, bool normalMap,
                                  const std::string& cacheDir);

// Textura já comprimida em DDS (BC7, cabeçalho DX10; usada por assets como o Bistro via MSFT_texture_dds): blocos e
// mips lidos como estão, sem recompressão. Falso se o arquivo não existe ou o formato não é BC7.
bool loadDds(const std::string& path, CompressedTexture& out);

// Nível `level` de uma textura BC7 decodificado para RGBA8 (para GPUs sem BC7 e --no-texture-compression).
std::vector<uint8_t> decodeBc7(const CompressedTexture& t, uint32_t level);

// PSNR (dB) do nível 0 decodificado contra o original (RGBA; xy nos normal maps) — medição de qualidade.
double compressionPsnr(const CompressedTexture& t, const uint8_t* rgba);

}  // namespace scene
