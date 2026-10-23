#include "rhi/pass.h"

#include "rhi/pipeline.h"

namespace rhi {

void PassLayout::init(Context& ctx, VkDescriptorSetLayout globalSet, std::initializer_list<VkDescriptorType> bindings,
                      uint32_t push) {
  pushSize = push;
  std::vector<VkDescriptorSetLayoutBinding> b;
  uint32_t i = 0;
  for (VkDescriptorType t : bindings) b.push_back({i++, t, 1, VK_SHADER_STAGE_ALL, nullptr});
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
  li.bindingCount = uint32_t(b.size());
  li.pBindings = b.data();
  VK_CHECK(vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &passSet));

  VkDescriptorSetLayout sets[] = {globalSet, passSet};
  VkPushConstantRange range{VK_SHADER_STAGE_ALL, 0, pushSize};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 2;
  pli.pSetLayouts = sets;
  pli.pushConstantRangeCount = pushSize ? 1 : 0;
  pli.pPushConstantRanges = &range;
  VK_CHECK(vkCreatePipelineLayout(ctx.device, &pli, nullptr, &pipelineLayout));
}

void PassLayout::bind(Context& ctx, VkCommandBuffer cmd, VkPipelineBindPoint point, VkDescriptorSet globalSet,
                      std::initializer_list<Binding> bindings, const void* push) const {
  vkCmdBindDescriptorSets(cmd, point, pipelineLayout, 0, 1, &globalSet, 0, nullptr);
  std::vector<VkWriteDescriptorSet> writes;
  writes.reserve(bindings.size());
  uint32_t i = 0;
  for (const Binding& b : bindings) {
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = i++;
    w.descriptorCount = 1;
    w.descriptorType = b.type;
    if (b.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) w.pBufferInfo = &b.buffer;
    else w.pImageInfo = &b.image;
    writes.push_back(w);
  }
  if (!writes.empty())
    ctx.vkCmdPushDescriptorSetKHR(cmd, point, pipelineLayout, 1, uint32_t(writes.size()), writes.data());
  if (pushSize && push) vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_ALL, 0, pushSize, push);
}

void PassLayout::destroy(Context& ctx) {
  vkDestroyPipelineLayout(ctx.device, pipelineLayout, nullptr);
  vkDestroyDescriptorSetLayout(ctx.device, passSet, nullptr);
  pipelineLayout = VK_NULL_HANDLE;
  passSet = VK_NULL_HANDLE;
}

void ComputePass::init(Context& ctx, const char* shader, VkDescriptorSetLayout globalSet,
                       std::initializer_list<VkDescriptorType> bindings, uint32_t pushSize) {
  ctx_ = &ctx;
  layout.init(ctx, globalSet, bindings, pushSize);
  VkShaderModule m = loadShader(ctx, shader);
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
  ci.layout = layout.pipelineLayout;
  VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_));
  vkDestroyShaderModule(ctx.device, m, nullptr);
}

void ComputePass::dispatch(VkCommandBuffer cmd, VkDescriptorSet globalSet, std::initializer_list<Binding> bindings,
                           VkExtent2D threads, const void* push) const {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  layout.bind(*ctx_, cmd, VK_PIPELINE_BIND_POINT_COMPUTE, globalSet, bindings, push);
  vkCmdDispatch(cmd, (threads.width + 7) / 8, (threads.height + 7) / 8, 1);
}

void ComputePass::dispatchGroups(VkCommandBuffer cmd, VkDescriptorSet globalSet, std::initializer_list<Binding> bindings,
                                 uint32_t groupsX, uint32_t groupsY, const void* push) const {
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  layout.bind(*ctx_, cmd, VK_PIPELINE_BIND_POINT_COMPUTE, globalSet, bindings, push);
  vkCmdDispatch(cmd, groupsX, groupsY, 1);
}

void ComputePass::destroy(Context& ctx) {
  if (pipeline_) vkDestroyPipeline(ctx.device, pipeline_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  layout.destroy(ctx);
}

}  // namespace rhi
