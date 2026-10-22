#include "renderer/renderer.h"

#include <cstring>

#include "rhi/pipeline.h"

namespace renderer {

namespace {

constexpr VkExtent2D kSkyLutExtent{256, 128};

// Layouts espelhados nos shaders (frame.glsl, forward.glsl, accumulate.comp, tonemap.frag).
struct FrameUniforms {
  glm::mat4 view, proj, viewProj, invViewProj;
  glm::vec4 cameraPos, sunDir, params;
  glm::uvec4 frameInfo;
};
struct InstanceGpu {
  uint64_t vertices, indices;
  glm::vec4 albedoRough, emissiveMetal;
  glm::uvec4 flags;
};
static_assert(sizeof(InstanceGpu) == 64);
struct ForwardPush {
  glm::mat4 model;
  glm::vec4 albedoRough, emissiveMetal;
  glm::uvec4 flags;
};
struct PostPush {
  uint32_t accumFrame;
  float exposure;
  uint32_t manualSrgb;
  uint32_t pad;
};

bool isSrgb(VkFormat f) {
  return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

VkPipeline createComputePipeline(rhi::Context& ctx, const char* shader, VkPipelineLayout layout) {
  VkShaderModule m = rhi::loadShader(ctx, shader);
  VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main"};
  ci.layout = layout;
  VkPipeline p;
  VK_CHECK(vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &p));
  vkDestroyShaderModule(ctx.device, m, nullptr);
  return p;
}

VkRenderingAttachmentInfo colorAttachment(VkImageView view, VkAttachmentLoadOp load) {
  VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  a.imageView = view;
  a.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  a.loadOp = load;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  return a;
}

void setViewport(VkCommandBuffer cmd, VkExtent2D e) {
  VkViewport vp{0, 0, float(e.width), float(e.height), 0, 1};
  VkRect2D sc{{0, 0}, e};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
}

}  // namespace

void Renderer::init(rhi::Context& ctx, scene::Scene& scene, VkExtent2D extent, VkFormat outputFormat) {
  ctx_ = &ctx;
  scene_ = &scene;
  extent_ = extent;
  outputFormat_ = outputFormat;
  VkDevice dev = ctx.device;

  // ---- Recursos persistentes ----
  skyLut_ = rhi::createImage2D(ctx, kSkyLutExtent, VK_FORMAT_R16G16B16A16_SFLOAT,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;  // azimute dá a volta
  si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VK_CHECK(vkCreateSampler(dev, &si, nullptr, &skySampler_));
  for (auto& b : frameUbo_)
    b = rhi::createBuffer(ctx, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);

  if (ctx.rayTracing) {
    std::vector<InstanceGpu> inst;
    for (uint32_t i = 0; i < scene.objects.size(); ++i) {
      const auto& o = scene.objects[i];
      const auto& m = scene.meshes[o.mesh];
      inst.push_back({m.vertices.address, m.indices.address, glm::vec4(o.material.albedo, o.material.roughness),
                      glm::vec4(o.material.emissive, o.material.metallic), glm::uvec4(i, o.material.type, 0, 0)});
    }
    instanceBuffer_ = rhi::createBufferWithData(ctx, inst.data(), inst.size() * sizeof(InstanceGpu),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }

  // ---- Descritores ----
  std::vector<VkDescriptorSetLayoutBinding> frameBindings = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
  };
  if (ctx.rayTracing) {
    frameBindings.push_back({3, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    frameBindings.push_back({4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
  }
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.bindingCount = uint32_t(frameBindings.size());
  li.pBindings = frameBindings.data();
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &li, nullptr, &frameLayout_));

  VkDescriptorSetLayoutBinding postBindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
  };
  li.bindingCount = 2;
  li.pBindings = postBindings;
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &li, nullptr, &postLayout_));

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kFramesInFlight + 2},
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight},
  };
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = kFramesInFlight + 1;
  pi.poolSizeCount = ctx.rayTracing ? 5 : 3;
  pi.pPoolSizes = sizes;
  VK_CHECK(vkCreateDescriptorPool(dev, &pi, nullptr, &pool_));

  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
  layouts.fill(frameLayout_);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = kFramesInFlight;
  ai.pSetLayouts = layouts.data();
  VK_CHECK(vkAllocateDescriptorSets(dev, &ai, frameSets_.data()));
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &postLayout_;
  VK_CHECK(vkAllocateDescriptorSets(dev, &ai, &postSet_));

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorBufferInfo ubo{frameUbo_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo lutSampled{skySampler_, skyLut_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo lutStorage{VK_NULL_HANDLE, skyLut_.view, VK_IMAGE_LAYOUT_GENERAL};
    std::vector<VkWriteDescriptorSet> w(3, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
    w[0].dstSet = frameSets_[i]; w[0].dstBinding = 0; w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[0].pBufferInfo = &ubo;
    w[1].dstSet = frameSets_[i]; w[1].dstBinding = 1; w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[1].pImageInfo = &lutSampled;
    w[2].dstSet = frameSets_[i]; w[2].dstBinding = 2; w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[2].pImageInfo = &lutStorage;

    VkWriteDescriptorSetAccelerationStructureKHR asWrite{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    VkDescriptorBufferInfo instInfo{instanceBuffer_.handle, 0, VK_WHOLE_SIZE};
    if (ctx.rayTracing) {
      asWrite.accelerationStructureCount = 1;
      asWrite.pAccelerationStructures = &scene.tlas.handle;
      VkWriteDescriptorSet a{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      a.pNext = &asWrite; a.dstSet = frameSets_[i]; a.dstBinding = 3; a.descriptorCount = 1;
      a.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
      VkWriteDescriptorSet b{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      b.dstSet = frameSets_[i]; b.dstBinding = 4; b.descriptorCount = 1;
      b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; b.pBufferInfo = &instInfo;
      w.push_back(a);
      w.push_back(b);
    }
    vkUpdateDescriptorSets(dev, uint32_t(w.size()), w.data(), 0, nullptr);
  }

  // ---- Pipelines ----
  VkPushConstantRange fwdPush{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ForwardPush)};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &frameLayout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &fwdPush;
  VK_CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &framePipelineLayout_));

  VkPushConstantRange postPush{VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PostPush)};
  pli.pSetLayouts = &postLayout_;
  pli.pPushConstantRanges = &postPush;
  VK_CHECK(vkCreatePipelineLayout(dev, &pli, nullptr, &postPipelineLayout_));

  skyLutPipeline_ = createComputePipeline(ctx, "skyview.comp", framePipelineLayout_);
  accumulatePipeline_ = createComputePipeline(ctx, "accumulate.comp", postPipelineLayout_);

  rhi::GraphicsPipelineDesc fwd;
  fwd.vertexShader = "forward.vert";
  fwd.fragmentShader = ctx.rayTracing ? "forward_rt.frag" : "forward.frag";
  fwd.layout = framePipelineLayout_;
  fwd.vertexBindings = {{0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
  fwd.vertexAttributes = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
  };
  fwd.colorFormats = {kHdrFormat};
  fwd.depthFormat = kDepthFormat;
  fwd.depthTest = fwd.depthWrite = true;
  fwd.cullMode = VK_CULL_MODE_BACK_BIT;
  forwardPipeline_ = rhi::createGraphicsPipeline(ctx, fwd);

  rhi::GraphicsPipelineDesc sky;
  sky.vertexShader = "fullscreen.vert";
  sky.fragmentShader = "sky.frag";
  sky.layout = framePipelineLayout_;
  sky.colorFormats = {kHdrFormat};
  sky.depthFormat = kDepthFormat;
  sky.depthTest = true;  // só onde a profundidade ainda é 0 (nada desenhado)
  skyPipeline_ = rhi::createGraphicsPipeline(ctx, sky);

  rhi::GraphicsPipelineDesc tm;
  tm.vertexShader = "fullscreen.vert";
  tm.fragmentShader = "tonemap.frag";
  tm.layout = postPipelineLayout_;
  tm.colorFormats = {outputFormat};
  tonemapPipeline_ = rhi::createGraphicsPipeline(ctx, tm);

  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    rhi::imageBarrier(cmd, skyLut_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  });
  createTargets();
}

void Renderer::createTargets() {
  rhi::Context& ctx = *ctx_;
  hdr_ = rhi::createImage2D(ctx, extent_, kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
  depth_ = rhi::createImage2D(ctx, extent_, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
  accum_ = rhi::createImage2D(ctx, extent_, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    rhi::imageBarrier(cmd, accum_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  });
  writePostDescriptors();
  accumFrame_ = 0;
}

void Renderer::writePostDescriptors() {
  VkDescriptorImageInfo hdrInfo{VK_NULL_HANDLE, hdr_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkDescriptorImageInfo accumInfo{VK_NULL_HANDLE, accum_.view, VK_IMAGE_LAYOUT_GENERAL};
  VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
  w[0].dstSet = postSet_; w[0].dstBinding = 0; w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[0].pImageInfo = &hdrInfo;
  w[1].dstSet = postSet_; w[1].dstBinding = 1; w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[1].pImageInfo = &accumInfo;
  vkUpdateDescriptorSets(ctx_->device, 2, w, 0, nullptr);
}

void Renderer::destroyTargets() {
  rhi::destroyImage(*ctx_, hdr_);
  rhi::destroyImage(*ctx_, depth_);
  rhi::destroyImage(*ctx_, accum_);
}

void Renderer::resize(VkExtent2D extent) {
  vkDeviceWaitIdle(ctx_->device);
  extent_ = extent;
  destroyTargets();
  createTargets();
}

void Renderer::record(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& cam, VkImage output, VkImageView outputView) {
  // ---- Uniforms do frame ----
  float aspect = float(extent_.width) / float(extent_.height);
  FrameUniforms u{};
  u.view = cam.view();
  u.proj = cam.projection(aspect);
  u.viewProj = u.proj * u.view;
  u.invViewProj = glm::inverse(u.viewProj);
  u.cameraPos = glm::vec4(cam.position, 0.0f);
  u.sunDir = glm::vec4(glm::normalize(scene_->sunDirection), scene_->sunIntensity);
  u.params = glm::vec4(settings.sunAngularRadius, settings.exposure, settings.fogDensity, settings.fogFalloff);
  u.frameInfo = glm::uvec4(frameCounter_++, ctx_->rayTracing ? 1u : 0u, 0u, 0u);
  std::memcpy(frameUbo_[slot].mapped, &u, sizeof(u));
  vmaFlushAllocation(ctx_->allocator, frameUbo_[slot].allocation, 0, VK_WHOLE_SIZE);

  // ---- 1. Sky-view LUT ----
  rhi::imageBarrier(cmd, skyLut_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, skyLutPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, framePipelineLayout_, 0, 1, &frameSets_[slot], 0, nullptr);
  vkCmdDispatch(cmd, (kSkyLutExtent.width + 7) / 8, (kSkyLutExtent.height + 7) / 8, 1);
  rhi::imageBarrier(cmd, skyLut_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

  // ---- 2+3. Forward HDR + céu ----
  rhi::imageBarrier(cmd, hdr_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  rhi::imageBarrier(cmd, depth_.handle, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

  VkRenderingAttachmentInfo color = colorAttachment(hdr_.view, VK_ATTACHMENT_LOAD_OP_CLEAR);
  VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  depth.imageView = depth_.view;
  depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth.clearValue.depthStencil = {0.0f, 0};  // reverse-Z: 0 = infinito
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, extent_};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = &depth;
  vkCmdBeginRendering(cmd, &ri);
  setViewport(cmd, extent_);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, forwardPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, framePipelineLayout_, 0, 1, &frameSets_[slot], 0, nullptr);
  for (uint32_t i = 0; i < scene_->objects.size(); ++i) {
    const auto& o = scene_->objects[i];
    const auto& m = scene_->meshes[o.mesh];
    ForwardPush p{o.transform, glm::vec4(o.material.albedo, o.material.roughness),
                  glm::vec4(o.material.emissive, o.material.metallic), glm::uvec4(i, o.material.type, 0, 0)};
    vkCmdPushConstants(cmd, framePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertices.handle, &offset);
    vkCmdBindIndexBuffer(cmd, m.indices.handle, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
  }
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);

  // ---- 4. Acumulação ----
  rhi::imageBarrier(cmd, hdr_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  rhi::imageBarrier(cmd, accum_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
  PostPush pp{accumFrame_, settings.exposure, isSrgb(outputFormat_) ? 0u : 1u, 0};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, accumulatePipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, postPipelineLayout_, 0, 1, &postSet_, 0, nullptr);
  vkCmdPushConstants(cmd, postPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pp), &pp);
  vkCmdDispatch(cmd, (extent_.width + 7) / 8, (extent_.height + 7) / 8, 1);
  rhi::imageBarrier(cmd, accum_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  accumFrame_++;

  // ---- 5. Tonemap -> saída ----
  rhi::imageBarrier(cmd, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo out = colorAttachment(outputView, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  ri.pColorAttachments = &out;
  ri.pDepthAttachment = nullptr;
  vkCmdBeginRendering(cmd, &ri);
  setViewport(cmd, extent_);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postPipelineLayout_, 0, 1, &postSet_, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
}

void Renderer::destroy() {
  if (!ctx_) return;
  VkDevice dev = ctx_->device;
  vkDeviceWaitIdle(dev);
  destroyTargets();
  rhi::destroyImage(*ctx_, skyLut_);
  vkDestroySampler(dev, skySampler_, nullptr);
  for (auto& b : frameUbo_) rhi::destroyBuffer(*ctx_, b);
  rhi::destroyBuffer(*ctx_, instanceBuffer_);
  for (VkPipeline p : {skyLutPipeline_, forwardPipeline_, skyPipeline_, accumulatePipeline_, tonemapPipeline_})
    vkDestroyPipeline(dev, p, nullptr);
  vkDestroyPipelineLayout(dev, framePipelineLayout_, nullptr);
  vkDestroyPipelineLayout(dev, postPipelineLayout_, nullptr);
  vkDestroyDescriptorPool(dev, pool_, nullptr);
  vkDestroyDescriptorSetLayout(dev, frameLayout_, nullptr);
  vkDestroyDescriptorSetLayout(dev, postLayout_, nullptr);
  ctx_ = nullptr;
}

}  // namespace renderer
