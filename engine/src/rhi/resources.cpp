#include "rhi/resources.h"

#include <algorithm>
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
  ctx.retire([&ctx, staging]() mutable { destroyBuffer(ctx, staging); });
  return dst;
}

void destroyBuffer(Context& ctx, Buffer& b) {
  if (b.handle) vmaDestroyBuffer(ctx.allocator, b.handle, b.allocation);
  b = {};
}

uint32_t mipCount(VkExtent2D e) {
  uint32_t n = 1;
  for (uint32_t s = std::max(e.width, e.height); s > 1; s >>= 1) ++n;
  return n;
}

namespace {
bool isDepthFormat(VkFormat format) {
  return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
         format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D16_UNORM;
}
VkImageView makeView(Context& ctx, const Image& img, uint32_t baseMip, uint32_t mips) {
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.handle;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = img.format;
  vi.subresourceRange = {VkImageAspectFlags(isDepthFormat(img.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                         baseMip, mips, 0, 1};
  VkImageView v;
  VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &v));
  return v;
}
}  // namespace

Image createImage2DArray(Context& ctx, VkExtent2D extent, uint32_t layers, VkFormat format, VkImageUsageFlags usage) {
  Image img;
  img.format = format;
  img.extent = extent;
  img.depth = layers;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {extent.width, extent.height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = layers;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
  VK_CHECK(vmaCreateImage(ctx.allocator, &ii, &ai, &img.handle, &img.allocation, nullptr));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.handle;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = format;
  vi.subresourceRange = {VkImageAspectFlags(isDepthFormat(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, layers};
  VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
  return img;
}

VkImageView createLayerView(Context& ctx, const Image& img, uint32_t layer) {
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.handle;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = img.format;
  vi.subresourceRange = {VkImageAspectFlags(isDepthFormat(img.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, layer, 1};
  VkImageView v;
  VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &v));
  return v;
}

Image createImage3D(Context& ctx, VkExtent3D extent, VkFormat format, VkImageUsageFlags usage) {
  Image img;
  img.format = format;
  img.extent = {extent.width, extent.height};
  img.depth = extent.depth;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_3D;
  ii.format = format;
  ii.extent = extent;
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  VK_CHECK(vmaCreateImage(ctx.allocator, &ii, &ai, &img.handle, &img.allocation, nullptr));
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = img.handle;
  vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &img.view));
  return img;
}

VkImageView createMipView(Context& ctx, const Image& img, uint32_t mip) { return makeView(ctx, img, mip, 1); }

Image createImage2D(Context& ctx, VkExtent2D extent, VkFormat format, VkImageUsageFlags usage, uint32_t mipLevels) {
  Image img;
  img.format = format;
  img.extent = extent;
  img.mipLevels = mipLevels;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = format;
  ii.extent = {extent.width, extent.height, 1};
  ii.mipLevels = mipLevels;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  VmaAllocationCreateInfo ai{};
  ai.usage = VMA_MEMORY_USAGE_AUTO;
  ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // render targets: memória dedicada
  VK_CHECK(vmaCreateImage(ctx.allocator, &ii, &ai, &img.handle, &img.allocation, nullptr));
  img.view = makeView(ctx, img, 0, mipLevels);
  return img;
}

Image createTexture(Context& ctx, const uint8_t* rgba, uint32_t w, uint32_t h, bool srgb) {
  VkExtent2D extent{w, h};
  uint32_t mips = mipCount(extent);
  Image img = createImage2D(ctx, extent, srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, mips);
  VkDeviceSize size = VkDeviceSize(w) * h * 4;
  Buffer staging = createBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
  std::memcpy(staging.mapped, rgba, size);
  vmaFlushAllocation(ctx.allocator, staging.allocation, 0, VK_WHOLE_SIZE);

  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    auto mipBarrier = [&](uint32_t mip, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                          VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = srcStage; b.srcAccessMask = srcAccess;
      b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
      b.oldLayout = from; b.newLayout = to;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = img.handle;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd, &dep);
    };
    imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging.handle, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Cada mip é gerado a partir do anterior com filtro linear.
    int32_t mw = int32_t(w), mh = int32_t(h);
    for (uint32_t i = 1; i < mips; ++i) {
      mipBarrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
      blit.srcOffsets[1] = {mw, mh, 1};
      mw = std::max(1, mw / 2);
      mh = std::max(1, mh / 2);
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
      blit.dstOffsets[1] = {mw, mh, 1};
      vkCmdBlitImage(cmd, img.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &blit, VK_FILTER_LINEAR);
      mipBarrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }
    mipBarrier(mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  });
  ctx.retire([&ctx, staging]() mutable { destroyBuffer(ctx, staging); });
  return img;
}

Image createCompressedTexture(Context& ctx, VkFormat format, uint32_t w, uint32_t h,
                              const std::vector<std::vector<uint8_t>>& mips) {
  Image img = createImage2D(ctx, {w, h}, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            uint32_t(mips.size()));
  VkDeviceSize total = 0;
  for (const auto& m : mips) total += m.size();
  Buffer staging = createBuffer(ctx, total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, MemoryUsage::CpuToGpu);
  std::vector<VkBufferImageCopy> regions;
  VkDeviceSize offset = 0;
  uint32_t mw = w, mh = h;
  for (uint32_t i = 0; i < mips.size(); ++i) {
    std::memcpy(static_cast<uint8_t*>(staging.mapped) + offset, mips[i].data(), mips[i].size());
    VkBufferImageCopy r{};
    r.bufferOffset = offset;  // múltiplo de 16: o tamanho de um bloco BC7
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    r.imageExtent = {mw, mh, 1};
    regions.push_back(r);
    offset += mips[i].size();
    mw = std::max(1u, mw / 2);
    mh = std::max(1u, mh / 2);
  }
  vmaFlushAllocation(ctx.allocator, staging.allocation, 0, VK_WHOLE_SIZE);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cmd, staging.handle, img.handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(regions.size()),
                           regions.data());
    imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
  });
  ctx.retire([&ctx, staging]() mutable { destroyBuffer(ctx, staging); });
  return img;
}

void memoryBarrier(VkCommandBuffer cmd) {
  VkMemoryBarrier2 b{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.memoryBarrierCount = 1;
  dep.pMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &dep);
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
