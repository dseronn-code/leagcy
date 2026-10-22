#pragma once
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
  VkImageView view = VK_NULL_HANDLE;
  VmaAllocation allocation = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
};

Image createImage2D(Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage);
void destroyImage(Context& ctx, Image& img);

// Barreira de layout de imagem (synchronization2) com estágios/acessos amplos e explícitos.
void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);

}  // namespace rhi
