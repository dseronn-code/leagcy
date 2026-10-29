#pragma once
#include <string>
#include <vector>

#include "rhi/context.h"

namespace rhi {

VkShaderModule loadShader(Context& ctx, const std::string& name);  // nome relativo a SHADER_DIR, ex.: "forward.vert"

// Builder de pipeline gráfico para dynamic rendering (sem VkRenderPass).
struct GraphicsPipelineDesc {
  std::string vertexShader;
  std::string fragmentShader;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  std::vector<VkVertexInputBindingDescription> vertexBindings;
  std::vector<VkVertexInputAttributeDescription> vertexAttributes;
  std::vector<VkFormat> colorFormats;
  VkFormat depthFormat = VK_FORMAT_UNDEFINED;
  bool depthTest = false;
  bool depthWrite = false;
  VkCompareOp depthCompare = VK_COMPARE_OP_GREATER_OR_EQUAL;  // reverse-Z
  VkCullModeFlags cullMode = VK_CULL_MODE_NONE;
  // Culling e sentido da frente escolhidos na hora do desenho (vkCmdSetCullMode/FrontFace, Vulkan 1.3).
  bool dynamicCulling = false;
};

VkPipeline createGraphicsPipeline(Context& ctx, const GraphicsPipelineDesc& desc);

}  // namespace rhi
