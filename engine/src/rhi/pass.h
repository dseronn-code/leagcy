#pragma once
#include <initializer_list>
#include <vector>

#include "rhi/resources.h"

namespace rhi {

// Um recurso passado a um passe via push descriptor (set 1 dos shaders).
// Imagens de render targets ficam sempre em VK_IMAGE_LAYOUT_GENERAL.
struct Binding {
  VkDescriptorType type;
  VkDescriptorImageInfo image{};
  VkDescriptorBufferInfo buffer{};

  static Binding storageImage(VkImageView v) {
    return {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL}, {}};
  }
  static Binding sampled(VkImageView v, VkSampler s) {
    return {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, {s, v, VK_IMAGE_LAYOUT_GENERAL}, {}};
  }
  static Binding storageBuffer(const Buffer& b) {
    return {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, {}, {b.handle, 0, VK_WHOLE_SIZE}};
  }
};

// Layout de um passe: set 0 = descritores globais do frame, set 1 = push descriptors do passe.
struct PassLayout {
  VkDescriptorSetLayout passSet = VK_NULL_HANDLE;
  VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
  uint32_t pushSize = 0;

  void init(Context& ctx, VkDescriptorSetLayout globalSet, std::initializer_list<VkDescriptorType> bindings, uint32_t pushSize);
  void bind(Context& ctx, VkCommandBuffer cmd, VkPipelineBindPoint point, VkDescriptorSet globalSet,
            std::initializer_list<Binding> bindings, const void* push) const;
  void destroy(Context& ctx);
};

// Passe de compute com grupos 8x8.
class ComputePass {
 public:
  void init(Context& ctx, const char* shader, VkDescriptorSetLayout globalSet,
            std::initializer_list<VkDescriptorType> bindings, uint32_t pushSize = 0);
  void dispatch(VkCommandBuffer cmd, VkDescriptorSet globalSet, std::initializer_list<Binding> bindings,
                VkExtent2D threads, const void* push = nullptr) const;
  // Número de grupos explícito (shaders com outro local_size, ex.: 64x1).
  void dispatchGroups(VkCommandBuffer cmd, VkDescriptorSet globalSet, std::initializer_list<Binding> bindings,
                      uint32_t groupsX, uint32_t groupsY, const void* push = nullptr) const;
  void destroy(Context& ctx);

  PassLayout layout;

 private:
  Context* ctx_ = nullptr;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace rhi
