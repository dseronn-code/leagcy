#include "rhi/pipeline.h"

#include <fstream>

namespace rhi {

VkShaderModule loadShader(Context& ctx, const std::string& name) {
  std::string path = std::string(SHADER_DIR) + "/" + name + ".spv";
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) FATAL("Shader não encontrado: %s", path.c_str());
  size_t size = size_t(f.tellg());
  std::vector<uint32_t> code(size / 4);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(code.data()), std::streamsize(size));
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = size;
  ci.pCode = code.data();
  VkShaderModule m;
  VK_CHECK(vkCreateShaderModule(ctx.device, &ci, nullptr, &m));
  return m;
}

VkPipeline createGraphicsPipeline(Context& ctx, const GraphicsPipelineDesc& d) {
  VkShaderModule vs = loadShader(ctx, d.vertexShader);
  VkShaderModule fs = loadShader(ctx, d.fragmentShader);
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};

  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = uint32_t(d.vertexBindings.size());
  vi.pVertexBindingDescriptions = d.vertexBindings.data();
  vi.vertexAttributeDescriptionCount = uint32_t(d.vertexAttributes.size());
  vi.pVertexAttributeDescriptions = d.vertexAttributes.data();

  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = d.cullMode;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = d.depthTest;
  ds.depthWriteEnable = d.depthWrite;
  ds.depthCompareOp = d.depthCompare;

  std::vector<VkPipelineColorBlendAttachmentState> blends(d.colorFormats.size());
  for (auto& b : blends)
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = uint32_t(blends.size());
  cb.pAttachments = blends.data();

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;

  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = uint32_t(d.colorFormats.size());
  ri.pColorAttachmentFormats = d.colorFormats.data();
  ri.depthAttachmentFormat = d.depthFormat;

  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pi.pNext = &ri;
  pi.stageCount = 2;
  pi.pStages = stages;
  pi.pVertexInputState = &vi;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &ds;
  pi.pColorBlendState = &cb;
  pi.pDynamicState = &dy;
  pi.layout = d.layout;
  VkPipeline p;
  VK_CHECK(vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pi, nullptr, &p));
  vkDestroyShaderModule(ctx.device, vs, nullptr);
  vkDestroyShaderModule(ctx.device, fs, nullptr);
  return p;
}

}  // namespace rhi
