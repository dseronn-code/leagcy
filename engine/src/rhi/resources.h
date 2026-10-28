#pragma once
#include <vector>
#include <cstddef>

#include "rhi/context.h"

namespace rhi {

struct Buffer {
  VkBuffer handle = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
  VkDeviceAddress address = 0;  // válido se criado com SHADER_DEVICE_ADDRESS
  void* mapped = nullptr;       // válido para buffers visíveis ao host
};

enum class MemoryUsage { GpuOnly, CpuToGpu, GpuToCpu };

Buffer createBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem);
// Cria um buffer GPU-only e copia `data` para ele via staging.
Buffer createBufferWithData(Context& ctx, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
void destroyBuffer(Context& ctx, Buffer& b);

struct Image {
  VkImage handle = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;  // todos os mips
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  uint32_t mipLevels = 1;
};

uint32_t mipCount(VkExtent2D extent);
Image createImage2D(Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, uint32_t mipLevels = 1);
// Volume 3D (ex.: perspectiva aérea em froxels). Um mip.
Image createImage3D(Context& ctx, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage);
// View de um único mip (para escrita em compute, ex.: cadeia do bloom). Quem chama destrói.
VkImageView createMipView(Context& ctx, const Image& img, uint32_t mip);
void destroyImage(Context& ctx, Image& img);

// Textura RGBA8 com cadeia completa de mipmaps gerada na GPU; termina em SHADER_READ_ONLY_OPTIMAL.
Image createTexture(Context& ctx, const uint8_t* rgba, uint32_t width, uint32_t height, bool srgb);
// Textura já comprimida (BC7) com todos os mips prontos: `mips[i]` = blocos do nível i, sem geração na GPU.
Image createCompressedTexture(Context& ctx, VkFormat format, uint32_t w, uint32_t h,
                              const std::vector<std::vector<uint8_t>>& mips);

// Barreira de memória global (todas as escritas -> todas as leituras). Simples e correta;
// um render graph poderá refinar isso no futuro.
void memoryBarrier(VkCommandBuffer cmd);

// Barreira de layout de imagem (synchronization2) com estágios/acessos amplos e explícitos.
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

}  // namespace rhi
