#include "renderer/renderer.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>

#include "rhi/pipeline.h"

namespace renderer {

namespace {

constexpr uint32_t kMaxTextures = 4096;
constexpr uint32_t kHistogramBins = 256;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_SRGB;
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;

constexpr VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
constexpr VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

// Espelha FrameData em shaders/frame.glsl (std140).
struct FrameUniforms {
  glm::mat4 view, proj, viewProj, invViewProj, viewProjNoJitter, prevViewProjNoJitter;
  glm::vec4 cameraPos, prevCameraPos, sunDir, params, jitter, renderSize, outputSize;
  glm::uvec4 frameInfo;
  glm::uvec4 quality0;  // spp, candidatos ReSTIR, vizinhos ReSTIR, amostras DOF
  glm::vec4 quality1;   // mip nos raios, limite de vagalumes, amostras de motion blur
};
static_assert(sizeof(FrameUniforms) == 544);

struct GBufferPush {
  uint32_t object;
  uint32_t material;
};
// Espelha ObjectData em shaders/scene.glsl.
struct ObjectGpu {
  glm::mat4 model, prevModel;
};
struct SkinningPush {
  uint64_t rest, skin, joints, out;
  uint32_t vertexCount, pad;
};
struct AtrousPush {
  int32_t stepSize;
  float phiColor, phiNormal, phiPlane;
};
struct ExposurePush {
  float compensation, manualEV100, speedUp, speedDown;
};
struct DofPush {
  float focusDistance, aperture, maxCoc, pad;
};
struct MotionBlurPush {
  float shutter, maxPixels, pad0, pad1;
};
struct BloomPush {
  glm::vec2 texel;
  uint32_t karisOrPad;
  float radius;
};
struct TonemapPush {
  float bloomStrength, bloomNorm;
  uint32_t manualSrgb;
  float pad;
};

bool isSrgb(VkFormat f) {
  return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

// Sequência de Halton (bases 2 e 3) para o jitter subpixel do TAA.
float halton(uint32_t i, uint32_t base) {
  float f = 1.0f, r = 0.0f;
  while (i > 0) {
    f /= float(base);
    r += f * float(i % base);
    i /= base;
  }
  return r;
}

VkSampler makeSampler(rhi::Context& ctx, VkFilter filter, VkSamplerAddressMode u, VkSamplerAddressMode v,
                      bool mips = false, float anisotropy = 0.0f) {
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = filter;
  si.mipmapMode = mips ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = u;
  si.addressModeV = si.addressModeW = v;
  si.maxLod = mips ? VK_LOD_CLAMP_NONE : 0.0f;
  si.anisotropyEnable = anisotropy > 0.0f;
  si.maxAnisotropy = anisotropy;
  VkSampler s;
  VK_CHECK(vkCreateSampler(ctx.device, &si, nullptr, &s));
  return s;
}

void setViewport(VkCommandBuffer cmd, VkExtent2D e) {
  VkViewport vp{0, 0, float(e.width), float(e.height), 0, 1};
  VkRect2D sc{{0, 0}, e};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);
}

VkRenderingAttachmentInfo attachment(VkImageView view, VkImageLayout layout, VkAttachmentLoadOp load) {
  VkRenderingAttachmentInfo a{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  a.imageView = view;
  a.imageLayout = layout;
  a.loadOp = load;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  return a;
}

}  // namespace

Quality qualityPreset(QualityPreset preset) {
  Quality q;  // Ultra
  switch (preset) {
    case QualityPreset::Low:
      q = {0.5f, 1, 4, 3, 3, 1, 3.0f, 8.0f, 256, 128, 5, 24, 8, 4.0f};
      break;
    case QualityPreset::Medium:
      q = {0.6f, 1, 8, 4, 4, 2, 2.0f, 12.0f, 256, 128, 6, 32, 12, 8.0f};
      break;
    case QualityPreset::High:
      q = {0.75f, 1, 12, 6, 5, 3, 1.0f, 16.0f, 384, 192, 6, 48, 12, 16.0f};
      break;
    case QualityPreset::Ultra:
      break;
    case QualityPreset::Cinema:
      q = {1.0f, 4, 32, 12, 5, 5, 0.0f, 64.0f, 1024, 512, 8, 128, 32, 16.0f};
      break;
  }
  return q;
}

bool parseQualityPreset(const char* name, QualityPreset& out) {
  const std::pair<const char*, QualityPreset> names[] = {{"baixo", QualityPreset::Low},   {"medio", QualityPreset::Medium},
                                                         {"alto", QualityPreset::High},   {"ultra", QualityPreset::Ultra},
                                                         {"cinema", QualityPreset::Cinema}};
  for (auto& [n, p] : names)
    if (std::strcmp(name, n) == 0) { out = p; return true; }
  return false;
}

void Renderer::init(rhi::Context& ctx, scene::Scene& scene, VkExtent2D outputExtent, VkFormat outputFormat,
                    const Settings& initialSettings) {
  ctx_ = &ctx;
  scene_ = &scene;
  settings = initialSettings;
  outputExtent_ = outputExtent;
  outputFormat_ = outputFormat;
  VkDevice dev = ctx.device;

  // ---- Recursos persistentes ----
  nearest_ = makeSampler(ctx, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  linear_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  skySampler_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
  const Quality& q = settings.quality;
  float anisotropy = std::min(q.anisotropy, ctx.properties.limits.maxSamplerAnisotropy);
  textureSampler_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, true, anisotropy);
  skyLutExtent_ = {q.skyLutWidth, q.skyLutHeight};
  skyLut_ = rhi::createImage2D(ctx, skyLutExtent_, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    frameUbo_[i] = rhi::createBuffer(ctx, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    objectBuffers_[i] = rhi::createBuffer(ctx, std::max<size_t>(1, scene.objects.size()) * sizeof(ObjectGpu),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    lightBuffers_[i] = rhi::createBuffer(ctx, std::max<size_t>(1, scene.lights.size()) * sizeof(scene::PointLight),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    jointBuffers_[i] = rhi::createBuffer(ctx, std::max<VkDeviceSize>(1, scene.totalJoints) * sizeof(glm::mat4),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         rhi::MemoryUsage::CpuToGpu);
  }
  std::vector<uint32_t> zeros(kHistogramBins, 0);
  histogram_ = rhi::createBufferWithData(ctx, zeros.data(), zeros.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  // Pré-exposição inicial de EV100 = 10 (entre interior e exterior); a adaptação converge no 2º frame.
  const float initialExposure = 1.0f / (1.2f * std::exp2(10.0f));
  glm::vec4 exposureInit(initialExposure, -1.0f, 10.0f, initialExposure);
  exposureState_ = rhi::createBufferWithData(ctx, &exposureInit, sizeof(exposureInit), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  // ---- Descritores globais (set 0) ----
  std::vector<VkDescriptorSetLayoutBinding> bindings = {
      {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, nullptr},
      {1, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {2, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {3, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {4, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {5, CIS, kMaxTextures, VK_SHADER_STAGE_ALL, nullptr},
      {7, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {8, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
  };
  if (ctx.rayTracing) bindings.push_back({6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_ALL, nullptr});
  std::vector<VkDescriptorBindingFlags> flags(bindings.size(), 0);
  flags[5] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;  // texturas bindless: só as existentes são escritas
  VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
  fi.bindingCount = uint32_t(flags.size());
  fi.pBindingFlags = flags.data();
  VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  li.pNext = &fi;
  li.bindingCount = uint32_t(bindings.size());
  li.pBindings = bindings.data();
  VK_CHECK(vkCreateDescriptorSetLayout(dev, &li, nullptr, &globalLayout_));

  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
      {CIS, kFramesInFlight * (1 + kMaxTextures)},
      {SB, kFramesInFlight * 5},
      {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, kFramesInFlight},
  };
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = kFramesInFlight;
  pi.poolSizeCount = ctx.rayTracing ? 4 : 3;
  pi.pPoolSizes = sizes;
  VK_CHECK(vkCreateDescriptorPool(dev, &pi, nullptr, &pool_));
  std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
  layouts.fill(globalLayout_);
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = kFramesInFlight;
  ai.pSetLayouts = layouts.data();
  VK_CHECK(vkAllocateDescriptorSets(dev, &ai, globalSets_.data()));
  writeGlobalDescriptors();

  // ---- Passes ----
  VkDescriptorSetLayout g = globalLayout_;
  const bool rt = ctx.rayTracing;
  skinningPass_.init(ctx, "skinning.comp", g, {}, sizeof(SkinningPush));
  skyLutPass_.init(ctx, "skyview.comp", g, {SI});
  restirTemporalPass_.init(ctx, rt ? "restir_temporal_rt.comp" : "restir_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, SB, SB});
  restirSpatialPass_.init(ctx, "restir_spatial.comp", g, {CIS, CIS, CIS, SB, SB});
  lightingPass_.init(ctx, rt ? "lighting_rt.comp" : "lighting.comp", g, {CIS, CIS, CIS, SB, SI, SI});
  svgfTemporalPass_.init(ctx, "svgf_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, SI, SI, SI, SI});
  svgfAtrousPass_.init(ctx, "svgf_atrous.comp", g, {CIS, CIS, CIS, CIS, SI}, sizeof(AtrousPush));
  compositePass_.init(ctx, "composite.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, SI});
  taaPass_.init(ctx, "taa.comp", g, {CIS, CIS, CIS, CIS, SI});
  histogramPass_.init(ctx, "exposure_histogram.comp", g, {CIS, SB});
  exposurePass_.init(ctx, "exposure_average.comp", g, {SB, SB}, sizeof(ExposurePush));
  dofPass_.init(ctx, "dof.comp", g, {CIS, CIS, SI}, sizeof(DofPush));
  motionBlurPass_.init(ctx, "motion_blur.comp", g, {CIS, CIS, SI}, sizeof(MotionBlurPush));
  bloomDownPass_.init(ctx, "bloom_down.comp", g, {CIS, SI}, sizeof(BloomPush));
  bloomUpPass_.init(ctx, "bloom_up.comp", g, {CIS, SI}, sizeof(BloomPush));

  gbufferLayout_.init(ctx, g, {}, sizeof(GBufferPush));
  rhi::GraphicsPipelineDesc gb;
  gb.vertexShader = "gbuffer.vert";
  gb.fragmentShader = "gbuffer.frag";
  gb.layout = gbufferLayout_.pipelineLayout;
  // Binding 1: vértices do frame anterior (só a posição é lida) para o movimento de malhas deformáveis.
  gb.vertexBindings = {{0, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX},
                       {1, sizeof(scene::Vertex), VK_VERTEX_INPUT_RATE_VERTEX}};
  gb.vertexAttributes = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(scene::Vertex, uv)},
      {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(scene::Vertex, tangent)},
      {4, 1, VK_FORMAT_R32G32B32_SFLOAT, offsetof(scene::Vertex, position)},
  };
  gb.colorFormats = {kAlbedoFormat, kHdrFormat, kMotionFormat, kHdrFormat};
  gb.depthFormat = kDepthFormat;
  gb.depthTest = gb.depthWrite = true;
  gb.cullMode = VK_CULL_MODE_NONE;  // materiais de dois lados (folhagem, tecidos)
  gbufferPipeline_ = rhi::createGraphicsPipeline(ctx, gb);

  tonemapLayout_.init(ctx, g, {CIS, CIS, SB}, sizeof(TonemapPush));
  rhi::GraphicsPipelineDesc tm;
  tm.vertexShader = "fullscreen.vert";
  tm.fragmentShader = "tonemap.frag";
  tm.layout = tonemapLayout_.pipelineLayout;
  tm.colorFormats = {outputFormat};
  tonemapPipeline_ = rhi::createGraphicsPipeline(ctx, tm);

  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    rhi::imageBarrier(cmd, skyLut_.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  });
  profiler_.init(ctx, kFramesInFlight);
  createTargets();
}

void Renderer::writeGlobalDescriptors() {
  scene::Scene& s = *scene_;
  std::vector<VkDescriptorImageInfo> texInfos;
  for (auto& t : s.textures) texInfos.push_back({textureSampler_, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
  if (texInfos.size() > kMaxTextures) FATAL("Cena com mais de %u texturas", kMaxTextures);

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorBufferInfo ubo{frameUbo_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo lut{skySampler_, skyLut_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufs[3] = {{s.materialBuffer.handle, 0, VK_WHOLE_SIZE},
                                      {s.instanceBuffer.handle, 0, VK_WHOLE_SIZE},
                                      {lightBuffers_[i].handle, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo objs{objectBuffers_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo exposure{exposureState_.handle, 0, VK_WHOLE_SIZE};
    std::vector<VkWriteDescriptorSet> w;
    auto add = [&](uint32_t binding, VkDescriptorType type, uint32_t count) -> VkWriteDescriptorSet& {
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      x.dstSet = globalSets_[i];
      x.dstBinding = binding;
      x.descriptorCount = count;
      x.descriptorType = type;
      w.push_back(x);
      return w.back();
    };
    add(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1).pBufferInfo = &ubo;
    add(1, CIS, 1).pImageInfo = &lut;
    for (uint32_t b = 0; b < 3; ++b) add(2 + b, SB, 1).pBufferInfo = &bufs[b];
    add(7, SB, 1).pBufferInfo = &objs;
    add(8, SB, 1).pBufferInfo = &exposure;
    if (!texInfos.empty()) add(5, CIS, uint32_t(texInfos.size())).pImageInfo = texInfos.data();
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    if (ctx_->rayTracing) {
      asw.accelerationStructureCount = 1;
      asw.pAccelerationStructures = &s.tlas.tlas.handle;
      add(6, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1).pNext = &asw;
    }
    vkUpdateDescriptorSets(ctx_->device, uint32_t(w.size()), w.data(), 0, nullptr);
  }
}

void Renderer::createTargets() {
  rhi::Context& ctx = *ctx_;
  float scale = glm::clamp(settings.quality.renderScale, 0.25f, 1.0f);
  renderExtent_ = {std::max(1u, uint32_t(float(outputExtent_.width) * scale + 0.5f)),
                   std::max(1u, uint32_t(float(outputExtent_.height) * scale + 0.5f))};
  const VkImageUsageFlags attach = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkExtent2D R = renderExtent_, O = outputExtent_;

  for (int i = 0; i < 2; ++i) {
    t_.depth[i] = rhi::createImage2D(ctx, R, kDepthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    t_.normal[i] = rhi::createImage2D(ctx, R, kHdrFormat, attach);
    t_.diffuseHistory[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.specularHistory[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.moments[i] = rhi::createImage2D(ctx, R, VK_FORMAT_R32G32B32A32_SFLOAT, storage);  // luminância² precisa de float32
    t_.historyLength[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.taa[i] = rhi::createImage2D(ctx, O, kHdrFormat, storage);
    t_.reservoirs[i] = rhi::createBuffer(ctx, VkDeviceSize(R.width) * R.height * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         rhi::MemoryUsage::GpuOnly);
  }
  t_.albedo = rhi::createImage2D(ctx, R, kAlbedoFormat, attach);
  t_.motion = rhi::createImage2D(ctx, R, kMotionFormat, attach);
  t_.emissive = rhi::createImage2D(ctx, R, kHdrFormat, attach);
  for (rhi::Image* img : {&t_.diffuseRaw, &t_.specularRaw, &t_.diffuseIntegrated, &t_.specularIntegrated, &t_.tmpA,
                          &t_.tmpB, &t_.tmpC, &t_.sceneColor})
    *img = rhi::createImage2D(ctx, R, kHdrFormat, storage);
  t_.postA = rhi::createImage2D(ctx, O, kHdrFormat, storage);
  t_.postB = rhi::createImage2D(ctx, O, kHdrFormat, storage);
  VkExtent2D bloomExtent{std::max(1u, O.width / 2), std::max(1u, O.height / 2)};
  t_.bloom = rhi::createImage2D(ctx, bloomExtent, kHdrFormat, storage, std::min(std::max(settings.quality.bloomMips, 1u), rhi::mipCount(bloomExtent)));
  for (uint32_t m = 0; m < t_.bloom.mipLevels; ++m) t_.bloomMips.push_back(rhi::createMipView(ctx, t_.bloom, m));

  // Tudo vive em GENERAL: o mesmo alvo é attachment, textura e storage em passes diferentes.
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    auto toGeneral = [&](rhi::Image& img, VkImageAspectFlags aspect) {
      rhi::imageBarrier(cmd, img.handle, aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_NONE,
                        0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    };
    for (int i = 0; i < 2; ++i) {
      toGeneral(t_.depth[i], VK_IMAGE_ASPECT_DEPTH_BIT);
      for (rhi::Image* img : {&t_.normal[i], &t_.diffuseHistory[i], &t_.specularHistory[i], &t_.moments[i],
                              &t_.historyLength[i], &t_.taa[i]})
        toGeneral(*img, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (rhi::Image* img : {&t_.albedo, &t_.motion, &t_.emissive, &t_.diffuseRaw, &t_.specularRaw, &t_.diffuseIntegrated,
                            &t_.specularIntegrated, &t_.tmpA, &t_.tmpB, &t_.tmpC, &t_.sceneColor, &t_.postA, &t_.postB,
                            &t_.bloom})
      toGeneral(*img, VK_IMAGE_ASPECT_COLOR_BIT);
  });
  historyValid_ = false;
}

void Renderer::destroyTargets() {
  rhi::Context& ctx = *ctx_;
  for (int i = 0; i < 2; ++i) {
    for (rhi::Image* img : {&t_.depth[i], &t_.normal[i], &t_.diffuseHistory[i], &t_.specularHistory[i], &t_.moments[i],
                            &t_.historyLength[i], &t_.taa[i]})
      rhi::destroyImage(ctx, *img);
    rhi::destroyBuffer(ctx, t_.reservoirs[i]);
  }
  for (rhi::Image* img : {&t_.albedo, &t_.motion, &t_.emissive, &t_.diffuseRaw, &t_.specularRaw, &t_.diffuseIntegrated,
                          &t_.specularIntegrated, &t_.tmpA, &t_.tmpB, &t_.tmpC, &t_.sceneColor, &t_.postA, &t_.postB,
                          &t_.bloom})
    rhi::destroyImage(ctx, *img);
  for (VkImageView v : t_.bloomMips) vkDestroyImageView(ctx.device, v, nullptr);
  t_.bloomMips.clear();
}

void Renderer::resize(VkExtent2D outputExtent) {
  vkDeviceWaitIdle(ctx_->device);
  outputExtent_ = outputExtent;
  destroyTargets();
  createTargets();
}

void Renderer::record(VkCommandBuffer cmd, uint32_t slot, const scene::Camera& cam, VkImage output, VkImageView outputView,
                      float dt) {
  using rhi::Binding;
  const uint32_t cur = uint32_t(frameIndex_ & 1), prev = cur ^ 1u;
  const VkExtent2D R = renderExtent_, O = outputExtent_;
  const VkDescriptorSet gs = globalSets_[slot];
  auto tex = [&](const rhi::Image& img) { return Binding::sampled(img.view, nearest_); };
  auto texLinear = [&](const rhi::Image& img) { return Binding::sampled(img.view, linear_); };
  auto storage = [&](const rhi::Image& img) { return Binding::storageImage(img.view); };
  auto barrier = [&]() { rhi::memoryBarrier(cmd); };

  // ---- Uniforms: jitter Halton (16 posições) em pixels internos ----
  uint32_t hi = uint32_t(frameIndex_ % 16) + 1;
  glm::vec2 jitterPx(halton(hi, 2) - 0.5f, halton(hi, 3) - 0.5f);
  glm::vec2 jitterUv = jitterPx / glm::vec2(R.width, R.height);
  float aspect = float(O.width) / float(O.height);
  FrameUniforms u{};
  u.view = cam.view();
  u.proj = cam.projection(aspect, jitterUv * 2.0f);
  u.viewProj = u.proj * u.view;
  u.invViewProj = glm::inverse(u.viewProj);
  u.viewProjNoJitter = cam.projection(aspect) * u.view;
  u.prevViewProjNoJitter = historyValid_ ? prevViewProj_ : u.viewProjNoJitter;
  u.cameraPos = glm::vec4(cam.position, cam.nearPlane);
  u.prevCameraPos = glm::vec4(historyValid_ ? prevCameraPos_ : cam.position, 0.0f);
  u.sunDir = glm::vec4(glm::normalize(scene_->sunDirection), scene_->sunIlluminance);
  u.params = glm::vec4(settings.sunAngularRadius, dt, scene_->fogDensity, scene_->fogFalloff);
  u.jitter = glm::vec4(jitterUv, prevJitter_);
  u.renderSize = glm::vec4(R.width, R.height, 1.0f / R.width, 1.0f / R.height);
  u.outputSize = glm::vec4(O.width, O.height, 1.0f / O.width, 1.0f / O.height);
  u.frameInfo = glm::uvec4(uint32_t(frameIndex_), ctx_->rayTracing ? 1u : 0u, uint32_t(scene_->lights.size()),
                           historyValid_ ? 1u : 0u);
  const Quality& q = settings.quality;
  u.quality0 = glm::uvec4(std::max(q.samplesPerPixel, 1u), std::max(q.restirCandidates, 1u), q.restirNeighbors,
                          std::max(q.dofSamples, 1u));
  u.quality1 = glm::vec4(q.rayTextureLod, q.fireflyLimit, float(std::max(q.motionBlurSamples, 1u)), 0.0f);
  std::memcpy(frameUbo_[slot].mapped, &u, sizeof(u));
  vmaFlushAllocation(ctx_->allocator, frameUbo_[slot].allocation, 0, VK_WHOLE_SIZE);

  barrier();  // ordena contra o frame anterior (mesmos alvos)

  profiler_.beginFrame(cmd, slot);
  // ---- 0. Cena dinâmica ----
  updateDynamicScene(cmd, slot);
  barrier();
  profiler_.mark(cmd, "cena");

  // ---- 1. Atmosfera ----
  skyLutPass_.dispatch(cmd, gs, {Binding::storageImage(skyLut_.view)}, skyLutExtent_);
  barrier();
  profiler_.mark(cmd, "ceu");

  // ---- 2. G-buffer ----
  {
    VkRenderingAttachmentInfo colors[4] = {
        attachment(t_.albedo.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.normal[cur].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.motion.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.emissive.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
    };
    VkRenderingAttachmentInfo depth = attachment(t_.depth[cur].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    depth.clearValue.depthStencil = {0.0f, 0};  // reverse-Z: 0 = infinito
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, R};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 4;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, R);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferLayout_.pipelineLayout, 0, 1, &gs, 0, nullptr);
    for (uint32_t i = 0; i < scene_->objects.size(); ++i) {
      const auto& o = scene_->objects[i];
      const auto& m = scene_->meshes[o.mesh];
      GBufferPush p{i, o.material};
      vkCmdPushConstants(cmd, gbufferLayout_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
      VkBuffer vbs[2] = {m.vertices.handle, m.skinned ? m.prevVertices.handle : m.vertices.handle};
      VkDeviceSize offsets[2] = {0, 0};
      vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offsets);
      vkCmdBindIndexBuffer(cmd, m.indices.handle, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRendering(cmd);
  }
  barrier();
  profiler_.mark(cmd, "gbuffer");

  const Binding gDepth = tex(t_.depth[cur]), gNormal = tex(t_.normal[cur]), gAlbedo = tex(t_.albedo);
  const Binding gDepthPrev = tex(t_.depth[prev]), gNormalPrev = tex(t_.normal[prev]), gMotion = tex(t_.motion);

  // ---- 3. ReSTIR DI ----
  restirTemporalPass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, gDepthPrev, gNormalPrev, gMotion,
                                         Binding::storageBuffer(t_.reservoirs[1]), Binding::storageBuffer(t_.reservoirs[0])}, R);
  barrier();
  restirSpatialPass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, Binding::storageBuffer(t_.reservoirs[0]),
                                        Binding::storageBuffer(t_.reservoirs[1])}, R);
  barrier();
  profiler_.mark(cmd, "restir");

  // ---- 4. Iluminação (ray tracing) ----
  lightingPass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, Binding::storageBuffer(t_.reservoirs[1]),
                                   storage(t_.diffuseRaw), storage(t_.specularRaw)}, R);
  barrier();
  profiler_.mark(cmd, "iluminacao_rt");

  // ---- 5. Denoiser ----
  const rhi::Image* diffuseFinal = &t_.diffuseRaw;
  const rhi::Image* specularFinal = &t_.specularRaw;
  if (settings.denoiser) {
    svgfTemporalPass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, gDepthPrev, gNormalPrev, gMotion, tex(t_.diffuseRaw),
                                         tex(t_.specularRaw), tex(t_.diffuseHistory[prev]), tex(t_.specularHistory[prev]),
                                         tex(t_.moments[prev]), tex(t_.historyLength[prev]), storage(t_.diffuseIntegrated),
                                         storage(t_.specularIntegrated), storage(t_.moments[cur]),
                                         storage(t_.historyLength[cur])}, R);
    barrier();
    // A 1ª iteração do à-trous vira o histórico do próximo frame (como no SVGF original).
    auto atrous = [&](const rhi::Image& in, const rhi::Image& out, int step, float phiColor) {
      AtrousPush p{step, phiColor, 64.0f, 0.02f};
      svgfAtrousPass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, tex(in), storage(out)}, R, &p);
      barrier();
    };
    // Difuso: integrado -> histórico -> A/B alternando, passo dobrando a cada iteração.
    uint32_t nd = std::max(settings.quality.denoiseDiffuseIterations, 1u);
    atrous(t_.diffuseIntegrated, t_.diffuseHistory[cur], 1, 4.0f);
    const rhi::Image* src = &t_.diffuseHistory[cur];
    for (uint32_t i = 1; i < nd; ++i) {
      const rhi::Image* dst = (i % 2 == 1) ? &t_.tmpA : &t_.tmpB;
      atrous(*src, *dst, 1 << i, 4.0f);
      src = dst;
    }
    diffuseFinal = src;
    // Especular: integrado -> histórico -> C/B alternando (A pode estar com o difuso final).
    uint32_t ns = std::max(settings.quality.denoiseSpecularIterations, 1u);
    atrous(t_.specularIntegrated, t_.specularHistory[cur], 1, 2.0f);
    src = &t_.specularHistory[cur];
    for (uint32_t i = 1; i < ns; ++i) {
      const rhi::Image* dst = (i % 2 == 1) ? &t_.tmpC : (diffuseFinal == &t_.tmpB ? &t_.tmpA : &t_.tmpB);
      atrous(*src, *dst, 1 << i, 2.0f);
      src = dst;
    }
    specularFinal = src;
    profiler_.mark(cmd, "denoiser");
  }

  // ---- 6. Composição ----
  compositePass_.dispatch(cmd, gs, {gDepth, gNormal, gAlbedo, tex(t_.emissive), tex(*diffuseFinal), tex(*specularFinal),
                                    storage(t_.sceneColor)}, R);
  barrier();
  profiler_.mark(cmd, "composicao");

  // ---- 7. TAA + upscaling ----
  taaPass_.dispatch(cmd, gs, {texLinear(t_.sceneColor), gDepth, gMotion, texLinear(t_.taa[prev]), storage(t_.taa[cur])}, O);
  barrier();
  profiler_.mark(cmd, "taa");
  const rhi::Image* color = &t_.taa[cur];

  // ---- 8. Auto-exposição ----
  histogramPass_.dispatch(cmd, gs, {tex(*color), Binding::storageBuffer(histogram_)}, O);
  barrier();
  ExposurePush ep{settings.exposureCompensation, settings.manualEV100, 3.0f, 1.5f};
  exposurePass_.dispatch(cmd, gs, {Binding::storageBuffer(histogram_), Binding::storageBuffer(exposureState_)}, {1, 1}, &ep);
  barrier();
  profiler_.mark(cmd, "exposicao");

  // ---- 9. Profundidade de campo / motion blur ----
  if (settings.depthOfField) {
    DofPush p{settings.dofFocusDistance, settings.dofAperture * float(O.height) / 1080.0f,
              settings.dofMaxCoc * float(O.height) / 1080.0f, 0};
    dofPass_.dispatch(cmd, gs, {texLinear(*color), gDepth, storage(t_.postA)}, O, &p);
    barrier();
    color = &t_.postA;
  }
  if (settings.motionBlur) {
    MotionBlurPush p{settings.shutter, 32.0f * float(O.height) / 1080.0f, 0, 0};
    motionBlurPass_.dispatch(cmd, gs, {texLinear(*color), gMotion, storage(t_.postB)}, O, &p);
    barrier();
    color = &t_.postB;
  }

  profiler_.mark(cmd, "dof_motionblur");
  // ---- 10. Bloom ----
  uint32_t mips = t_.bloom.mipLevels;
  auto mipExtent = [&](uint32_t m) {
    return VkExtent2D{std::max(1u, t_.bloom.extent.width >> m), std::max(1u, t_.bloom.extent.height >> m)};
  };
  for (uint32_t m = 0; m < mips; ++m) {
    VkExtent2D src = m == 0 ? O : mipExtent(m - 1);
    BloomPush p{glm::vec2(1.0f / src.width, 1.0f / src.height), m == 0 ? 1u : 0u, 0.0f};
    Binding in = m == 0 ? texLinear(*color) : Binding::sampled(t_.bloomMips[m - 1], linear_);
    bloomDownPass_.dispatch(cmd, gs, {in, Binding::storageImage(t_.bloomMips[m])}, mipExtent(m), &p);
    barrier();
  }
  for (uint32_t m = mips - 1; m-- > 0;) {
    VkExtent2D lower = mipExtent(m + 1);
    BloomPush p{glm::vec2(1.0f / lower.width, 1.0f / lower.height), 0u, 1.0f};
    bloomUpPass_.dispatch(cmd, gs, {Binding::sampled(t_.bloomMips[m + 1], linear_), Binding::storageImage(t_.bloomMips[m])},
                          mipExtent(m), &p);
    barrier();
  }

  profiler_.mark(cmd, "bloom");
  // ---- 11. Tonemap -> saída ----
  rhi::imageBarrier(cmd, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  VkRenderingAttachmentInfo out = attachment(outputView, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, O};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &out;
  vkCmdBeginRendering(cmd, &ri);
  setViewport(cmd, O);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
  TonemapPush tp{settings.bloomStrength, 1.0f / float(mips), isSrgb(outputFormat_) ? 0u : 1u, 0};
  tonemapLayout_.bind(*ctx_, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gs,
                      {tex(*color), Binding::sampled(t_.bloomMips[0], linear_), Binding::storageBuffer(exposureState_)}, &tp);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  profiler_.mark(cmd, "tonemap");

  prevViewProj_ = u.viewProjNoJitter;
  prevCameraPos_ = cam.position;
  prevJitter_ = jitterUv;
  historyValid_ = true;
  ++frameIndex_;
}

void Renderer::updateDynamicScene(VkCommandBuffer cmd, uint32_t slot) {
  scene::Scene& s = *scene_;
  rhi::Context& ctx = *ctx_;

  // Transformações atual e anterior de cada objeto (vetores de movimento por objeto).
  auto* objs = static_cast<ObjectGpu*>(objectBuffers_[slot].mapped);
  for (size_t i = 0; i < s.objects.size(); ++i)
    objs[i] = {s.objects[i].transform, historyValid_ ? s.objects[i].prevTransform : s.objects[i].transform};
  vmaFlushAllocation(ctx.allocator, objectBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);

  // Luzes (podem se mover: faróis, tochas carregadas, etc.).
  if (!s.lights.empty()) {
    std::memcpy(lightBuffers_[slot].mapped, s.lights.data(), s.lights.size() * sizeof(scene::PointLight));
    vmaFlushAllocation(ctx.allocator, lightBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);
  }

  // Matrizes das juntas de todos os esqueletos.
  auto* joints = static_cast<glm::mat4*>(jointBuffers_[slot].mapped);
  bool anySkinned = false;
  for (const auto& m : s.meshes) {
    if (!m.skinned) continue;
    anySkinned = true;
    const auto& src = s.models[size_t(m.skinModel)].jointMatrices[size_t(m.skin)];
    std::copy(src.begin(), src.end(), joints + m.jointOffset);
  }
  vmaFlushAllocation(ctx.allocator, jointBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);

  if (anySkinned) {
    // Guarda a pose anterior antes de sobrescrever (para os vetores de movimento da deformação).
    if (historyValid_) {
      for (const auto& m : s.meshes) {
        if (!m.skinned) continue;
        VkBufferCopy region{0, 0, m.vertices.size};
        vkCmdCopyBuffer(cmd, m.vertices.handle, m.prevVertices.handle, 1, &region);
      }
      rhi::memoryBarrier(cmd);
    }
    for (const auto& m : s.meshes) {
      if (!m.skinned) continue;
      SkinningPush p{m.restVertices.address, m.skinData.address,
                     jointBuffers_[slot].address + VkDeviceAddress(m.jointOffset) * sizeof(glm::mat4), m.vertices.address,
                     m.vertexCount, 0};
      skinningPass_.dispatchGroups(cmd, globalSets_[slot], {}, (m.vertexCount + 63) / 64, 1, &p);
    }
    rhi::memoryBarrier(cmd);
    if (!historyValid_) {  // primeiro frame: anterior = atual
      for (const auto& m : s.meshes) {
        if (!m.skinned) continue;
        VkBufferCopy region{0, 0, m.vertices.size};
        vkCmdCopyBuffer(cmd, m.vertices.handle, m.prevVertices.handle, 1, &region);
      }
    }
    if (ctx.rayTracing) {
      for (auto& m : s.meshes) {
        if (!m.skinned) continue;
        rhi::BlasInput in{m.vertices.address, m.vertexCount, sizeof(scene::Vertex), m.indices.address, m.indexCount};
        rhi::recordBlasUpdate(ctx, cmd, m.blas, in, rhi::alignScratch(ctx, s.skinScratch.address + m.updateScratchOffset));
      }
      rhi::memoryBarrier(cmd);
    }
  }
  if (ctx.rayTracing) s.tlas.record(cmd, slot, s.tlasInstances());
}

void Renderer::destroy() {
  if (!ctx_) return;
  rhi::Context& ctx = *ctx_;
  vkDeviceWaitIdle(ctx.device);
  destroyTargets();
  profiler_.destroy();
  rhi::destroyImage(ctx, skyLut_);
  for (VkSampler s : {nearest_, linear_, skySampler_, textureSampler_}) vkDestroySampler(ctx.device, s, nullptr);
  for (auto* arr : {&frameUbo_, &objectBuffers_, &jointBuffers_, &lightBuffers_})
    for (auto& b : *arr) rhi::destroyBuffer(ctx, b);
  rhi::destroyBuffer(ctx, histogram_);
  rhi::destroyBuffer(ctx, exposureState_);
  for (rhi::ComputePass* p : {&skinningPass_, &skyLutPass_, &restirTemporalPass_, &restirSpatialPass_, &lightingPass_, &svgfTemporalPass_,
                              &svgfAtrousPass_, &compositePass_, &taaPass_, &histogramPass_, &exposurePass_, &dofPass_,
                              &motionBlurPass_, &bloomDownPass_, &bloomUpPass_})
    p->destroy(ctx);
  vkDestroyPipeline(ctx.device, gbufferPipeline_, nullptr);
  vkDestroyPipeline(ctx.device, tonemapPipeline_, nullptr);
  gbufferLayout_.destroy(ctx);
  tonemapLayout_.destroy(ctx);
  vkDestroyDescriptorPool(ctx.device, pool_, nullptr);
  vkDestroyDescriptorSetLayout(ctx.device, globalLayout_, nullptr);
  ctx_ = nullptr;
}

}  // namespace renderer
