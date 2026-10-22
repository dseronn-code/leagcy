#include "rhi/resources.h"

#include <cstring>

namespace rhi {

Buffer createBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, MemoryUsage mem) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = usage;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  if (mem == MemoryUsage::CpuToGpu)
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  else if (mem == MemoryUsage::GpuToCpu)
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
  VmaAllocationInfo info{};
  VK_CHECK(vmaCreateBuffer(ctx.allocator, &bi, &ai, &b.handle, &b.allocation, &info));
  b.mapped = info.pMappedData;
  if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) {
    VkBufferDeviceAddressInfo ai2{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai2.buffer = b.handle;
    b.address = vkGetBufferDeviceAddress(ctx.device, &ai2);
  }
  return b;
}

Buffer createBufferWithData(Context& ctx, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
  Buffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
  std::memcpy(staging.mapped, data, size);
  vmaFlushAllocation(ctx.allocator, staging.allocation, 0, VK_WHOLE_SIZE);
  Buffer dst = createBuffer(ctx, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, MemoryUsage::GpuOnly);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.handle, dst.handle, 1, &region);
  });
  destroyBuffer(ctx, staging);
  return dst;
}

void destroyBuffer(Context& ctx, Buffer& b) {
  if (b.handle) vmaDestroyBuffer(ctx.allocator, b.handle, b.allocation);
  b = {};
}

Image createImage2D(Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage) {
  Image img;
  img.format = format;
  img.extent = extent;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {extent.width, extent.height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // render targets: memória dedicada
  VK_CHECK(vmaCreateImage(ctx.allocator, &ii, &ai, &img.handle, &img.allocation, nullptr));

  bool depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
               format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.handle;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
  return img;
}

void destroyImage(Context& ctx, Image& img) {
  if (img.view) vkDestroyImageView(ctx.device, img.view, nullptr);
  if (img.handle) vmaDestroyImage(ctx.allocator, img.handle, img.allocation);
  img = {};
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                  VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = srcStage;
  b.srcAccessMask = srcAccess;
  b.dstStageMask = dstStage;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace rhi
