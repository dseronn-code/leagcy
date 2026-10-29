#include "renderer/renderer.h"

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstring>
#include <tuple>
#include <cstdio>

#include "rhi/pipeline.h"

namespace renderer {

namespace {

constexpr uint32_t kMaxTextures = 4096;
constexpr uint32_t kHistogramBins = 256;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkExtent3D kCloudNoiseExtent = {128, 128, 128};
constexpr VkExtent2D kCloudShadowExtent = {256, 256};
constexpr VkFormat kAlbedoFormat = VK_FORMAT_R8G8B8A8_SRGB;  // cor difusa e f0 (sRGB: precisão nos tons baixos)
constexpr VkExtent2D kDfgLutExtent{128, 128};
constexpr VkExtent2D kTransmittanceLutExtent{256, 64};
constexpr VkExtent2D kMultiScatterLutExtent{32, 32};
constexpr VkExtent3D kAerialPerspectiveExtent{32, 32, 32};
constexpr VkFormat kMotionFormat = VK_FORMAT_R16G16_SFLOAT;
// Espelha RC_SIZE e CacheEntry em shaders/radiance_cache.glsl.
constexpr uint32_t kRadianceCacheEntries = 1u << 20;
constexpr VkDeviceSize kRadianceCacheBytes = VkDeviceSize(kRadianceCacheEntries) * 48;
constexpr VkFormat kVisibilityFormat = VK_FORMAT_R32G32_UINT;

constexpr VkDescriptorType CIS = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
constexpr VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

// Espelha FrameData em shaders/frame.glsl (std140).
struct FrameUniforms {
  glm::mat4 view, proj, viewProj, invViewProj, viewProjNoJitter, prevViewProjNoJitter, prevInvViewProj;
  glm::vec4 cameraPos, prevCameraPos, sunDir, params, jitter, renderSize, outputSize;
  glm::uvec4 frameInfo;
  glm::uvec4 quality0;  // spp, candidatos ReSTIR, vizinhos ReSTIR, amostras DOF
  glm::vec4 quality1;   // mip nos raios, limite de vagalumes, amostras de motion blur, compensação de energia
  glm::uvec4 quality2;  // x = correção de viés do ReSTIR, y = sinal no formato do ReBLUR, z = mip bias (bits de float)
  glm::vec4 environment;  // x = céu uniforme (nits), y = modo de depuração, z = exposição da depuração,
                          // w = reusos do ReSTIR desligados (bit 0 temporal, bit 1 espacial)
  glm::vec4 night;        // x = 1: a luz direcional é a lua; y = iluminância de uma estrela de magnitude 0 (lux)
};
static_assert(sizeof(FrameUniforms) == 656);

// Frente dos triângulos: anti-horário, como no glTF (a projeção inverte o Y e o viewport do Vulkan também: as duas
// inversões se cancelam — conferido pela imagem, com culling o chão e as paredes continuam lá).
constexpr bool kFrontFaceClockwise = false;

struct GBufferPush {
  VkDeviceAddress drawList;  // lista de objetos ordenada por malha (shaders/gbuffer.vert)
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
  uint32_t debugView;  // 1 = mostra o valor cru (linear) sem exposição, bloom nem tonemap
  float sharpness;     // RCAS
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
      q = {0.5f, 1, 4, 3, 3, 1, 3.0f, 8.0f, 256, 128, 5, 24, 8, 4.0f, false};
      break;
    case QualityPreset::Medium:
      q = {0.6f, 1, 8, 4, 4, 2, 2.0f, 12.0f, 256, 128, 6, 32, 12, 8.0f, false};
      break;
    case QualityPreset::High:
      q = {0.75f, 1, 12, 6, 5, 3, 1.0f, 16.0f, 384, 192, 6, 48, 12, 16.0f, true};
      break;
    case QualityPreset::Ultra:
      break;
    case QualityPreset::Cinema:
      q = {1.0f, 4, 32, 12, 5, 5, 0.0f, 64.0f, 1024, 512, 8, 128, 32, 16.0f, true};
      break;
  }
  return q;
}

bool parseDebugView(const char* name, uint32_t& out) {
  const std::pair<const char*, uint32_t> names[] = {{"sol", kDebugSunVisibility}, {"sol-luz", kDebugSunShading},
                                                    {"normal", kDebugGeometricNormal}, {"nl", kDebugSunCosine},
                                                    {"luzes", kDebugLocalLights}, {"luzes-ref", kDebugLocalLightsReference}};
  for (auto& [n, v] : names)
    if (std::strcmp(name, n) == 0) { out = v; return true; }
  return false;
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
  dfgLut_ = rhi::createImage2D(ctx, kDfgLutExtent, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  transmittanceLut_ = rhi::createImage2D(ctx, kTransmittanceLutExtent, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  multiScatterLut_ = rhi::createImage2D(ctx, kMultiScatterLutExtent, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  aerialPerspective_ = rhi::createImage3D(ctx, kAerialPerspectiveExtent, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  cloudNoise_ = rhi::createImage3D(ctx, kCloudNoiseExtent, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  skyClear_ = rhi::createImage2D(ctx, skyLutExtent_, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  cloudShadow_ = rhi::createImage2D(ctx, kCloudShadowExtent, kHdrFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  cloudSampler_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    frameUbo_[i] = rhi::createBuffer(ctx, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    objectBuffers_[i] = rhi::createBuffer(ctx, std::max<size_t>(1, scene.objects.size()) * sizeof(ObjectGpu),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    lightBuffers_[i] = rhi::createBuffer(ctx, std::max<size_t>(1, scene.lights.size()) * sizeof(scene::PointLight),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    lightBvhBuffers_[i] = rhi::createBuffer(ctx, std::max<size_t>(1, 2 * scene.lights.size()) * sizeof(LightBvhNode),
                                            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    jointBuffers_[i] = rhi::createBuffer(ctx, std::max<VkDeviceSize>(1, scene.totalJoints) * sizeof(glm::mat4),
                                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                         rhi::MemoryUsage::CpuToGpu);
  }
  {
    std::vector<uint32_t> order(scene.objects.size());
    for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    // Lotes por (dois lados, espelhado, malha). Um lote espelhado inverte a frente; objetos que passem a espelhar
    // depois do início (raro) só desenham do lado errado se forem de um lado só.
    auto key = [&](uint32_t i) {
      const auto& o = scene.objects[i];
      const bool ds = (scene.materials[o.material].flags & scene::kMaterialDoubleSided) != 0;
      const bool mirrored = glm::determinant(glm::mat3(o.transform)) < 0.0f;
      return std::make_tuple(ds, mirrored, o.mesh);
    };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
    for (uint32_t k = 0; k < order.size(); ++k) {
      const auto [ds, mirrored, mesh] = key(order[k]);
      if (drawBatches_.empty() || drawBatches_.back().mesh != mesh || drawBatches_.back().doubleSided != ds ||
          drawBatches_.back().mirrored != mirrored)
        drawBatches_.push_back({mesh, k, 0, ds, mirrored});
      ++drawBatches_.back().count;
    }
    if (order.empty()) order.push_back(0);
    drawList_ = rhi::createBufferWithData(ctx, order.data(), order.size() * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  }
  std::vector<uint32_t> zeros(kHistogramBins, 0);
  histogram_ = rhi::createBufferWithData(ctx, zeros.data(), zeros.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  // Pré-exposição inicial de EV100 = 10 (entre interior e exterior); a adaptação converge no 2º frame.
  const float initialExposure = 1.0f / (1.2f * std::exp2(10.0f));
  glm::vec4 exposureInit(initialExposure, -1.0f, 10.0f, initialExposure);
  exposureState_ = rhi::createBufferWithData(ctx, &exposureInit, sizeof(exposureInit), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  // Cache de radiância: 2^20 células de 48 bytes, começa vazio (chave 0).
  radianceCache_ = rhi::createBuffer(ctx, kRadianceCacheBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     rhi::MemoryUsage::GpuOnly);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    vkCmdFillBuffer(cmd, radianceCache_.handle, 0, VK_WHOLE_SIZE, 0);
    rhi::memoryBarrier(cmd);
  });

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
      {9, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {10, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {11, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {12, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {13, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {14, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {15, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {16, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
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
      {CIS, kFramesInFlight * (7 + kMaxTextures)},
      {SB, kFramesInFlight * 7},
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
  aerialPerspectivePass_.init(ctx, "aerial_perspective.comp", g, {SI});
  cloudSkyPass_.init(ctx, "clouds_sky.comp", g, {SI, CIS});
  cloudShadowPass_.init(ctx, "cloud_shadow.comp", g, {SI});
  cloudScreenPass_.init(ctx, "clouds_screen.comp", g, {SI, CIS, CIS});
  restirTemporalPass_.init(ctx, rt ? "restir_temporal_rt.comp" : "restir_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, SB, SB, CIS, CIS});
  restirSpatialPass_.init(ctx, rt ? "restir_spatial_rt.comp" : "restir_spatial.comp", g, {CIS, CIS, CIS, CIS, SB, SB, CIS});
  lightingPass_.init(ctx, rt ? "lighting_rt.comp" : "lighting.comp", g, {CIS, CIS, CIS, CIS, SB, SI, SI, CIS, SI, SI});
  svgfTemporalPass_.init(ctx, "svgf_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, SI, SI, SI, SI});
  svgfAtrousPass_.init(ctx, "svgf_atrous.comp", g, {CIS, CIS, CIS, CIS, CIS, SI}, sizeof(AtrousPush));
  compositePass_.init(ctx, "composite.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, SI, CIS, CIS});
  taaPass_.init(ctx, "taa.comp", g, {CIS, CIS, CIS, CIS, SI});
  rrGuidesPass_.init(ctx, "rr_guides.comp", g, {CIS, CIS, CIS, CIS, CIS, SI, SI, SI, SI});
  accumulatePass_.init(ctx, "accumulate.comp", g, {CIS, CIS, CIS, CIS, CIS, SI, SI}, sizeof(uint32_t));
  radianceCacheResolvePass_.init(ctx, "radiance_cache_resolve.comp", g, {});
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
  gb.colorFormats = {kAlbedoFormat, kHdrFormat, kMotionFormat, kHdrFormat, kAlbedoFormat, kVisibilityFormat};
  gb.depthFormat = kDepthFormat;
  gb.depthTest = gb.depthWrite = true;
  gb.dynamicCulling = true;  // por lote: de um lado só (padrão do glTF) ou dos dois (doubleSided: folhagem, tecidos)
  gbufferPipeline_ = rhi::createGraphicsPipeline(ctx, gb);

  tonemapLayout_.init(ctx, g, {CIS, CIS, SB}, sizeof(TonemapPush));
  rhi::GraphicsPipelineDesc tm;
  tm.vertexShader = "fullscreen.vert";
  tm.fragmentShader = "tonemap.frag";
  tm.layout = tonemapLayout_.pipelineLayout;
  tm.colorFormats = {outputFormat};
  tonemapPipeline_ = rhi::createGraphicsPipeline(ctx, tm);

  // LUTs constantes, calculadas uma vez: DFG (integral do BRDF), transmitância e múltiplo
  // espalhamento da atmosfera (esta depende da de transmitância).
  rhi::ComputePass dfgPass, transmittancePass, multiScatterPass, cloudNoisePass;
  dfgPass.init(ctx, "dfg_lut.comp", g, {SI});
  cloudNoisePass.init(ctx, "cloud_noise.comp", g, {SI});
  transmittancePass.init(ctx, "atmosphere_transmittance.comp", g, {SI});
  multiScatterPass.init(ctx, "atmosphere_multiscatter.comp", g, {SI});
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    for (rhi::Image* img : {&skyLut_, &dfgLut_, &transmittanceLut_, &multiScatterLut_, &aerialPerspective_, &cloudNoise_,
                            &skyClear_, &cloudShadow_})
      rhi::imageBarrier(cmd, img->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    dfgPass.dispatch(cmd, globalSets_[0], {rhi::Binding::storageImage(dfgLut_.view)}, kDfgLutExtent);
    transmittancePass.dispatch(cmd, globalSets_[0], {rhi::Binding::storageImage(transmittanceLut_.view)}, kTransmittanceLutExtent);
    rhi::memoryBarrier(cmd);
    multiScatterPass.dispatch(cmd, globalSets_[0], {rhi::Binding::storageImage(multiScatterLut_.view)}, kMultiScatterLutExtent);
    cloudNoisePass.dispatch(cmd, globalSets_[0], {rhi::Binding::storageImage(cloudNoise_.view)},
                            {kCloudNoiseExtent.width, kCloudNoiseExtent.height});
    rhi::memoryBarrier(cmd);
  });
  for (rhi::ComputePass* p : {&dfgPass, &transmittancePass, &multiScatterPass, &cloudNoisePass}) p->destroy(ctx);
  profiler_.init(ctx, kFramesInFlight);
  createTargets();
}

void Renderer::writeGlobalDescriptors() {
  scene::Scene& s = *scene_;
  std::vector<VkDescriptorImageInfo> texInfos;
  for (size_t i = 0; i < s.textures.size(); ++i) {
    // Um sampler por modo de repetição do glTF (no máximo 9), com o mesmo filtro anisotrópico.
    const auto wrap = i < s.textureWrap.size() ? s.textureWrap[i]
                                               : std::make_pair(VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    VkSampler& smp = wrapSamplers_[wrap];
    if (!smp) {
      const float anisotropy = std::min(settings.quality.anisotropy, ctx_->properties.limits.maxSamplerAnisotropy);
      smp = wrap.first == VK_SAMPLER_ADDRESS_MODE_REPEAT && wrap.second == VK_SAMPLER_ADDRESS_MODE_REPEAT
                ? textureSampler_
                : makeSampler(*ctx_, VK_FILTER_LINEAR, wrap.first, wrap.second, true, anisotropy);
    }
    texInfos.push_back({smp, s.textures[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
  }
  if (texInfos.size() > kMaxTextures) FATAL("Cena com mais de %u texturas", kMaxTextures);

  for (uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkDescriptorBufferInfo ubo{frameUbo_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo lut{skySampler_, skyLut_.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo bufs[3] = {{s.materialBuffer.handle, 0, VK_WHOLE_SIZE},
                                      {s.instanceBuffer.handle, 0, VK_WHOLE_SIZE},
                                      {lightBuffers_[i].handle, 0, VK_WHOLE_SIZE}};
    VkDescriptorBufferInfo objs{objectBuffers_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo exposure{exposureState_.handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo lightBvh{lightBvhBuffers_[i].handle, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo cache{radianceCache_.handle, 0, VK_WHOLE_SIZE};
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
    add(13, SB, 1).pBufferInfo = &lightBvh;
    add(14, SB, 1).pBufferInfo = &cache;
    VkDescriptorImageInfo dfg{linear_, dfgLut_.view, VK_IMAGE_LAYOUT_GENERAL};
    add(9, CIS, 1).pImageInfo = &dfg;
    VkDescriptorImageInfo atmo[3] = {{linear_, transmittanceLut_.view, VK_IMAGE_LAYOUT_GENERAL},
                                     {linear_, multiScatterLut_.view, VK_IMAGE_LAYOUT_GENERAL},
                                     {linear_, aerialPerspective_.view, VK_IMAGE_LAYOUT_GENERAL}};
    for (uint32_t b = 0; b < 3; ++b) add(10 + b, CIS, 1).pImageInfo = &atmo[b];
    VkDescriptorImageInfo clouds[2] = {{cloudSampler_, cloudNoise_.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {linear_, cloudShadow_.view, VK_IMAGE_LAYOUT_GENERAL}};
    for (uint32_t b = 0; b < 2; ++b) add(15 + b, CIS, 1).pImageInfo = &clouds[b];
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
  // DLSS/FSR: o fabricante define a resolução interna do modo mais próximo da escala pedida.
  // Padrão (Auto) numa RTX com o denoiser do motor ligado: DLSS Ray Reconstruction — medido na RTX 3070 Ti contra a
  // referência verdadeira, melhor que SVGF e ReLAX em erro, brilhinhos e cintilação (docs/auditoria.md, item 5).
  const bool autoDenoise = settings.autoRayReconstruction && ctx.rayTracing && settings.denoiser && !settings.nrd &&
                           !settings.referenceAccumulation && settings.debugView == 0;
  upscalerReady_ = settings.upscaler != Upscaler::Kind::None &&
                   upscaler_.init(ctx, settings.upscaler, outputExtent_, scale, autoDenoise);
  if (upscalerReady_) renderExtent_ = upscaler_.renderExtent();
  const VkImageUsageFlags attach = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkExtent2D R = renderExtent_, O = outputExtent_;
  // Imagens entregues ao DLSS/FSR: o NGX limpa algumas com vkCmdClearColorImage (validação na RTX 3070 Ti).
  const VkImageUsageFlags upscalerIo = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  for (int i = 0; i < 2; ++i) {
    t_.depth[i] = rhi::createImage2D(ctx, R, kDepthFormat,
                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | upscalerIo);
    t_.normal[i] = rhi::createImage2D(ctx, R, kHdrFormat, attach);
    t_.visibility[i] = rhi::createImage2D(ctx, R, kVisibilityFormat, attach);
    t_.diffuseHistory[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.specularHistory[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.moments[i] = rhi::createImage2D(ctx, R, VK_FORMAT_R32G32B32A32_SFLOAT, storage);  // luminância² precisa de float32
    t_.historyLength[i] = rhi::createImage2D(ctx, R, kHdrFormat, storage);
    t_.taa[i] = rhi::createImage2D(ctx, O, kHdrFormat, storage | upscalerIo);
    t_.reservoirs[i] = rhi::createBuffer(ctx, VkDeviceSize(R.width) * R.height * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                         rhi::MemoryUsage::GpuOnly);
  }
  t_.diffuse = rhi::createImage2D(ctx, R, kAlbedoFormat, attach);
  t_.specular = rhi::createImage2D(ctx, R, kAlbedoFormat, attach);
  // STORAGE: o NRD cria views de storage de todas as entradas (exige shaderStorageImageExtendedFormats).
  t_.motion = rhi::createImage2D(ctx, R, kMotionFormat, attach | VK_IMAGE_USAGE_STORAGE_BIT | upscalerIo);
  t_.emissive = rhi::createImage2D(ctx, R, kHdrFormat, attach);
  for (rhi::Image* img : {&t_.diffuseRaw, &t_.specularRaw, &t_.diffuseIntegrated, &t_.specularIntegrated, &t_.tmpA,
                          &t_.tmpB, &t_.tmpC})
    *img = rhi::createImage2D(ctx, R, kHdrFormat, storage);
  t_.sceneColor = rhi::createImage2D(ctx, R, kHdrFormat, storage | upscalerIo);
  t_.viewZ = rhi::createImage2D(ctx, R, VK_FORMAT_R32_SFLOAT, storage);
  t_.nrdNormalRoughness = rhi::createImage2D(ctx, R, VK_FORMAT_A2B10G10R10_UNORM_PACK32, storage);
  t_.diffuseDenoised = rhi::createImage2D(ctx, R, kHdrFormat, storage);
  t_.specularDenoised = rhi::createImage2D(ctx, R, kHdrFormat, storage);
  t_.nrdValidation = rhi::createImage2D(ctx, R, VK_FORMAT_R8G8B8A8_UNORM, storage);
  for (rhi::Image* img : {&t_.rrDiffuseAlbedo, &t_.rrSpecularAlbedo, &t_.rrNormalRoughness})
    *img = rhi::createImage2D(ctx, R, kHdrFormat, storage | upscalerIo);
  t_.rrHitDistance = rhi::createImage2D(ctx, R, VK_FORMAT_R16_SFLOAT, storage | upscalerIo);
  t_.accumulation = rhi::createImage2D(ctx, R, VK_FORMAT_R32G32B32A32_SFLOAT, storage);
  for (auto& c : t_.clouds)
    c = rhi::createImage2D(ctx, {std::max(1u, R.width / 2), std::max(1u, R.height / 2)}, kHdrFormat, storage);
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
      for (rhi::Image* img : {&t_.normal[i], &t_.visibility[i], &t_.diffuseHistory[i], &t_.specularHistory[i], &t_.moments[i],
                              &t_.historyLength[i], &t_.taa[i]})
        toGeneral(*img, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (rhi::Image* img : {&t_.diffuse, &t_.specular, &t_.motion, &t_.emissive, &t_.diffuseRaw, &t_.specularRaw,
                            &t_.diffuseIntegrated, &t_.specularIntegrated, &t_.tmpA, &t_.tmpB, &t_.tmpC, &t_.sceneColor, &t_.postA,
                            &t_.postB, &t_.bloom, &t_.viewZ, &t_.nrdNormalRoughness, &t_.diffuseDenoised,
                            &t_.specularDenoised, &t_.nrdValidation, &t_.rrDiffuseAlbedo,
                            &t_.rrSpecularAlbedo, &t_.rrHitDistance, &t_.rrNormalRoughness, &t_.accumulation, &t_.clouds[0],
                            &t_.clouds[1]})
      toGeneral(*img, VK_IMAGE_ASPECT_COLOR_BIT);
  });
  if (upscalerReady_) {
    auto img = [](const rhi::Image& i, VkImageUsageFlags usage) { return Upscaler::Image{i.handle, i.format, i.extent, usage}; };
    Upscaler::Inputs in{};
    in.color = img(t_.sceneColor, storage | upscalerIo);
    for (int i = 0; i < 2; ++i)
      in.depth[i] = img(t_.depth[i], VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | upscalerIo);
    in.motion = img(t_.motion, attach | VK_IMAGE_USAGE_STORAGE_BIT | upscalerIo);
    in.output[0] = img(t_.taa[0], storage | upscalerIo);
    in.output[1] = img(t_.taa[1], storage | upscalerIo);
    in.normalRoughness = img(t_.rrNormalRoughness, storage | upscalerIo);
    in.diffuseAlbedo = img(t_.rrDiffuseAlbedo, storage | upscalerIo);
    in.specularAlbedo = img(t_.rrSpecularAlbedo, storage | upscalerIo);
    in.specularHitDistance = img(t_.rrHitDistance, storage | upscalerIo);
    upscalerReady_ = upscaler_.bind(in);
  }
  historyValid_ = false;
  // O NRD cria texturas internas do tamanho da resolução de render: recriado junto com os alvos.
  nrdReady_ = nrd_.init(ctx, R, kFramesInFlight, settings.nrdReblur);
}

void Renderer::destroyTargets() {
  rhi::Context& ctx = *ctx_;
  nrd_.destroy();
  nrdReady_ = false;
  upscaler_.destroy();
  upscalerReady_ = false;
  for (int i = 0; i < 2; ++i) {
    for (rhi::Image* img : {&t_.depth[i], &t_.normal[i], &t_.visibility[i], &t_.diffuseHistory[i], &t_.specularHistory[i], &t_.moments[i],
                            &t_.historyLength[i], &t_.taa[i]})
      rhi::destroyImage(ctx, *img);
    rhi::destroyBuffer(ctx, t_.reservoirs[i]);
  }
  for (rhi::Image* img : {&t_.diffuse, &t_.specular, &t_.motion, &t_.emissive, &t_.diffuseRaw, &t_.specularRaw,
                          &t_.diffuseIntegrated, &t_.specularIntegrated, &t_.tmpA, &t_.tmpB, &t_.tmpC, &t_.sceneColor, &t_.postA,
                          &t_.postB, &t_.bloom, &t_.viewZ, &t_.nrdNormalRoughness, &t_.diffuseDenoised,
                          &t_.specularDenoised, &t_.nrdValidation, &t_.rrDiffuseAlbedo,
                            &t_.rrSpecularAlbedo, &t_.rrHitDistance, &t_.rrNormalRoughness, &t_.accumulation, &t_.clouds[0],
                            &t_.clouds[1]})
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

  // ---- Uniforms: jitter Halton (16 posições, ou as que o DLSS/FSR pedem) em pixels internos ----
  uint32_t hi = uint32_t(frameIndex_ % (upscalerReady_ ? upscaler_.jitterPhases() : 16u)) + 1;
  glm::vec2 jitterPx(halton(hi, 2) - 0.5f, halton(hi, 3) - 0.5f);
  glm::vec2 jitterUv = jitterPx / glm::vec2(R.width, R.height);
  float aspect = float(O.width) / float(O.height);
  FrameUniforms u{};
  u.view = cam.view();
  u.proj = cam.projection(aspect, jitterUv * 2.0f);
  u.viewProj = u.proj * u.view;
  // Inversa relativa à câmera (sem translação): ver worldPosFromDepth em frame.glsl.
  u.invViewProj = glm::inverse(u.proj * glm::mat4(glm::mat3(u.view)));
  u.viewProjNoJitter = cam.projection(aspect) * u.view;
  u.prevViewProjNoJitter = historyValid_ ? prevViewProj_ : u.viewProjNoJitter;
  u.prevInvViewProj = historyValid_ ? prevInvViewProj_ : u.invViewProj;
  u.cameraPos = glm::vec4(cam.position, cam.nearPlane);
  u.prevCameraPos = glm::vec4(historyValid_ ? prevCameraPos_ : cam.position, 0.0f);
  u.sunDir = glm::vec4(glm::normalize(scene_->sunDirection), scene_->sunIlluminance);
  // Noite: a lua faz o papel do sol (mesmo disco, sombra e céu espalhado, com a iluminância dela) e as estrelas
  // aparecem no céu. Iluminância de magnitude 0 no topo da atmosfera: 2,5 µlux.
  u.night = glm::vec4(0.0f, 0.0f, scene_->cloudCoverage, scene_->time);
  if (scene_->sunIlluminance <= 0.0f) {
    u.night.y = 2.5e-6f;
    if (scene_->moonIlluminance > 0.0f) {
      u.sunDir = glm::vec4(glm::normalize(scene_->moonDirection), scene_->moonIlluminance);
      u.night.x = 1.0f;
    }
  }
  u.params = glm::vec4(settings.sunAngularRadius, dt, scene_->fogDensity, scene_->fogFalloff);
  u.jitter = glm::vec4(jitterUv, prevJitter_);
  u.renderSize = glm::vec4(R.width, R.height, 1.0f / R.width, 1.0f / R.height);
  u.outputSize = glm::vec4(O.width, O.height, 1.0f / O.width, 1.0f / O.height);
  u.frameInfo = glm::uvec4(uint32_t(frameIndex_), ctx_->rayTracing ? 1u : 0u, uint32_t(scene_->lights.size()),
                           historyValid_ ? 1u : 0u);
  const Quality& q = settings.quality;
  u.quality0 = glm::uvec4(std::max(q.samplesPerPixel, 1u), std::max(q.restirCandidates, 1u), q.restirNeighbors,
                          std::max(q.dofSamples, 1u));
  u.quality1 = glm::vec4(q.rayTextureLod, q.fireflyLimit, float(std::max(q.motionBlurSamples, 1u)),
                         settings.energyCompensation ? 1.0f : 0.0f);
  const bool reblurSignal = settings.denoiser && settings.nrd && settings.nrdReblur && settings.debugView == kDebugNone;
  // Mip bias das texturas de material: o DLSS/FSR reconstrói na resolução de saída e pede texturas mais nítidas.
  const float mipBias = upscalerReady_ ? upscaler_.mipBias() : 0.0f;
  u.quality2 = glm::uvec4(q.restirBiasCorrection ? 1u : 0u, reblurSignal ? 1u : 0u, glm::floatBitsToUint(mipBias),
                          (settings.referencePathTrace ? 1u : 0u) | (settings.radianceCache ? 0u : 2u) |
                              (settings.virtualMotion ? 0u : 4u));
  // z: exposição das visualizações de luz (EV100 manual; EV 0 sem --exposure).
  float debugEV = std::isnan(settings.manualEV100) ? 0.0f : settings.manualEV100;
  u.environment = glm::vec4(scene_->uniformSkyLuminance, float(settings.debugView), std::exp2(-debugEV) / 1.2f,
                            float(settings.restirDisableMask));
  std::memcpy(frameUbo_[slot].mapped, &u, sizeof(u));
  vmaFlushAllocation(ctx_->allocator, frameUbo_[slot].allocation, 0, VK_WHOLE_SIZE);

  barrier();  // ordena contra o frame anterior (mesmos alvos)

  profiler_.beginFrame(cmd, slot);
  // ---- 0. Cena dinâmica ----
  cpuLast_ = std::chrono::steady_clock::now();
  ++cpuFrames_;
  updateDynamicScene(cmd, slot);
  barrier();
  profiler_.mark(cmd, "cena");

  // ---- 1. Atmosfera (e nuvens: o céu limpo vai para skyClear_ e as nuvens o compõem na LUT final) ----
  const bool clouds = scene_->cloudCoverage > 0.0f && scene_->uniformSkyLuminance <= 0.0f;
  skyLutPass_.dispatch(cmd, gs, {Binding::storageImage(clouds ? skyClear_.view : skyLut_.view)}, skyLutExtent_);
  aerialPerspectivePass_.dispatch(cmd, gs, {Binding::storageImage(aerialPerspective_.view)},
                                  {kAerialPerspectiveExtent.width, kAerialPerspectiveExtent.height});
  if (clouds) cloudShadowPass_.dispatch(cmd, gs, {Binding::storageImage(cloudShadow_.view)}, kCloudShadowExtent);
  barrier();
  if (clouds) {
    cloudSkyPass_.dispatch(cmd, gs, {Binding::storageImage(skyLut_.view), Binding::sampled(skyClear_.view, linear_)},
                           skyLutExtent_);
    cloudScreenPass_.dispatch(cmd, gs, {Binding::storageImage(t_.clouds[cur].view), Binding::sampled(t_.clouds[cur ^ 1u].view, linear_),
                                        Binding::sampled(skyClear_.view, linear_)},
                              t_.clouds[cur].extent);
    barrier();
  }
  profiler_.mark(cmd, "ceu");

  // ---- 2. G-buffer ----
  {
    VkRenderingAttachmentInfo colors[6] = {
        attachment(t_.diffuse.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.normal[cur].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.motion.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.emissive.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.specular.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),
        attachment(t_.visibility[cur].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR),  // 0 = sem objeto
    };
    VkRenderingAttachmentInfo depth = attachment(t_.depth[cur].view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
    depth.clearValue.depthStencil = {0.0f, 0};  // reverse-Z: 0 = infinito
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, R};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 6;
    ri.pColorAttachments = colors;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, R);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gbufferLayout_.pipelineLayout, 0, 1, &gs, 0, nullptr);
    // Uma chamada por malha com todas as suas instâncias (medido na cidade: 98 mil chamadas, uma por objeto, custavam
    // ~25 ms de CPU por frame).
    GBufferPush p{drawList_.address};
    vkCmdPushConstants(cmd, gbufferLayout_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
    for (const DrawBatch& b : drawBatches_) {
      const auto& m = scene_->meshes[b.mesh];
      vkCmdSetCullMode(cmd, b.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
      vkCmdSetFrontFace(cmd, b.mirrored != kFrontFaceClockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
      VkBuffer vbs[2] = {m.vertices.handle, m.skinned ? m.prevVertices.handle : m.vertices.handle};
      VkDeviceSize offsets[2] = {0, 0};
      vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offsets);
      vkCmdBindIndexBuffer(cmd, m.indices.handle, 0, VK_INDEX_TYPE_UINT32);
      vkCmdDrawIndexed(cmd, m.indexCount, b.count, 0, 0, b.first);
    }
    vkCmdEndRendering(cmd);
  }
  cpuMark("desenho");
  barrier();
  profiler_.mark(cmd, "gbuffer");

  const Binding gDepth = tex(t_.depth[cur]), gNormal = tex(t_.normal[cur]), gDiffuse = tex(t_.diffuse),
                gSpecular = tex(t_.specular);
  const Binding gDepthPrev = tex(t_.depth[prev]), gNormalPrev = tex(t_.normal[prev]), gMotion = tex(t_.motion);

  // ---- 3. ReSTIR DI ----
  restirTemporalPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, gDepthPrev, gNormalPrev, gMotion,
                                         Binding::storageBuffer(t_.reservoirs[1]), Binding::storageBuffer(t_.reservoirs[0]),
                                         tex(t_.visibility[cur]), tex(t_.visibility[prev])}, R);
  barrier();
  restirSpatialPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, Binding::storageBuffer(t_.reservoirs[0]),
                                        Binding::storageBuffer(t_.reservoirs[1]), tex(t_.visibility[cur])}, R);
  barrier();
  profiler_.mark(cmd, "restir");

  // ---- 4. Iluminação (ray tracing) ----
  lightingPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, Binding::storageBuffer(t_.reservoirs[1]),
                                   storage(t_.diffuseRaw), storage(t_.specularRaw), tex(t_.visibility[cur]),
                                   storage(t_.viewZ), storage(t_.nrdNormalRoughness)}, R);
  barrier();
  if (ctx_->rayTracing) {  // fecha o frame do cache de radiância (média das amostras -> histórico)
    radianceCacheResolvePass_.dispatch(cmd, gs, {}, {1024, kRadianceCacheEntries / 1024});
    barrier();
  }
  profiler_.mark(cmd, "iluminacao_rt");

  // Depuração: o valor cru da iluminação vai direto para a tela (sem denoiser, TAA, exposição nem tonemap).
  const bool debug = settings.debugView != kDebugNone;
  bool showValidation = false;  // camada de validação do NRD no lugar da imagem final
  const rhi::Image* color = &t_.diffuseRaw;
  uint32_t mips = t_.bloom.mipLevels;
  if (!debug) {
  // ---- 5. Denoiser ----
  const rhi::Image* diffuseFinal = &t_.diffuseRaw;
  const rhi::Image* specularFinal = &t_.specularRaw;
  if (settings.denoiser && settings.nrd && nrdReady_ && nrd_.reblur() != settings.nrdReblur)
    nrdReady_ = nrd_.init(*ctx_, R, kFramesInFlight, settings.nrdReblur);  // trocou de denoiser
  if (upscalerReady_ && upscaler_.denoises()) {
    // DLSS Ray Reconstruction: a iluminação vai ruidosa para a composição; aqui só as imagens-guia.
    rrGuidesPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.specularRaw), storage(t_.rrDiffuseAlbedo),
                                     storage(t_.rrSpecularAlbedo), storage(t_.rrHitDistance), storage(t_.rrNormalRoughness)}, R);
    barrier();
    profiler_.mark(cmd, "denoiser");
  } else if (settings.denoiser && settings.nrd && nrdReady_) {
    // NVIDIA NRD ReLAX: entradas em GENERAL, o NRD faz as próprias barreiras e devolve tudo em GENERAL.
    NrdDenoiser::Inputs in;
    in.motion = {t_.motion.handle, kMotionFormat};
    in.normalRoughness = {t_.nrdNormalRoughness.handle, VK_FORMAT_A2B10G10R10_UNORM_PACK32};
    in.viewZ = {t_.viewZ.handle, VK_FORMAT_R32_SFLOAT};
    in.diffuse = {t_.diffuseRaw.handle, kHdrFormat};
    in.specular = {t_.specularRaw.handle, kHdrFormat};
    in.outDiffuse = {t_.diffuseDenoised.handle, kHdrFormat};
    in.outSpecular = {t_.specularDenoised.handle, kHdrFormat};
    if (settings.nrdValidation) in.validation = {t_.nrdValidation.handle, VK_FORMAT_R8G8B8A8_UNORM};
    NrdDenoiser::Frame f;
    f.view = u.view;
    f.proj = cam.projection(aspect);
    f.prevView = historyValid_ ? prevView_ : f.view;
    f.prevProj = historyValid_ ? prevProjNoJitter_ : f.proj;
    // Nossa projeção desloca a imagem em +jitter: o pixel amostra a cena em uv − jitter (convenção do NRD).
    f.jitterPixels = -jitterPx;
    f.prevJitterPixels = historyValid_ ? prevJitterPixels_ : f.jitterPixels;
    f.frameIndex = uint32_t(frameIndex_);
    f.frameTimeMs = dt * 1000.0f;
    f.resetHistory = !historyValid_;
    nrd_.denoise(cmd, in, f);
    barrier();
    diffuseFinal = &t_.diffuseDenoised;
    specularFinal = &t_.specularDenoised;
    profiler_.mark(cmd, "denoiser");
    if (settings.nrdValidation) {
      showValidation = true;
      color = &t_.nrdValidation;
    }
  } else if (settings.denoiser) {
    svgfTemporalPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, gDepthPrev, gNormalPrev, gMotion, tex(t_.diffuseRaw),
                                         tex(t_.specularRaw), tex(t_.diffuseHistory[prev]), tex(t_.specularHistory[prev]),
                                         tex(t_.moments[prev]), tex(t_.historyLength[prev]), storage(t_.diffuseIntegrated),
                                         storage(t_.specularIntegrated), storage(t_.moments[cur]),
                                         storage(t_.historyLength[cur])}, R);
    barrier();
    // A 1ª iteração do à-trous vira o histórico do próximo frame (como no SVGF original).
    auto atrous = [&](const rhi::Image& in, const rhi::Image& out, int step, float phiColor) {
      AtrousPush p{step, phiColor, 64.0f, 0.02f};
      svgfAtrousPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(in), storage(out)}, R, &p);
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

  if (!showValidation) {
  // ---- 6. Composição ----
  compositePass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.emissive), tex(*diffuseFinal), tex(*specularFinal),
                                    storage(t_.sceneColor), Binding::sampled(t_.clouds[cur].view, linear_),
                                    Binding::sampled(skyClear_.view, linear_)}, R);
  barrier();
  profiler_.mark(cmd, "composicao");

  // ---- 7. TAA + upscaling (do motor, ou DLSS / FSR) ----
  if (settings.referenceAccumulation) {
    // Referência: zera quando a câmera se move; senão soma mais um frame.
    const bool still = historyValid_ && u.view == prevView_;
    accumulatedFrames_ = still ? accumulatedFrames_ + 1 : 1;
    accumulatePass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.sceneColor), storage(t_.accumulation),
                                       storage(t_.taa[cur])}, R, &accumulatedFrames_);
  } else if (upscalerReady_) {
    Upscaler::Frame uf{};
    uf.jitterPixels = jitterPx;
    uf.zNear = cam.nearPlane;
    uf.verticalFov = cam.fovY;
    uf.frameTimeMs = dt * 1000.0f;
    uf.index = cur;
    uf.resetHistory = !historyValid_;
    uf.view = u.view;
    uf.projNoJitter = cam.projection(aspect);
    upscaler_.dispatch(cmd, uf);
  } else {
    taaPass_.dispatch(cmd, gs, {texLinear(t_.sceneColor), gDepth, gMotion, texLinear(t_.taa[prev]), storage(t_.taa[cur])}, O);
  }
  barrier();
  profiler_.mark(cmd, "taa");
  color = &t_.taa[cur];

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
  }  // !showValidation
  }  // !debug
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
  const bool dlss = upscalerReady_ && (upscaler_.kind() == Upscaler::Kind::Dlss || upscaler_.kind() == Upscaler::Kind::DlssRR);
  const float sharpness = settings.sharpness >= 0.0f ? settings.sharpness : (dlss ? 0.0f : 0.5f);
  TonemapPush tp{settings.bloomStrength, 1.0f / float(mips), isSrgb(outputFormat_) ? 0u : 1u, (debug || showValidation) ? 1u : 0u,
                 sharpness};
  tonemapLayout_.bind(*ctx_, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gs,
                      {tex(*color), Binding::sampled(t_.bloomMips[0], linear_), Binding::storageBuffer(exposureState_)}, &tp);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
  profiler_.mark(cmd, "tonemap");

  cpuMark("passes");
  prevViewProj_ = u.viewProjNoJitter;
  prevInvViewProj_ = u.invViewProj;
  prevView_ = u.view;
  prevProjNoJitter_ = cam.projection(aspect);
  prevJitterPixels_ = -jitterPx;
  prevCameraPos_ = cam.position;
  prevJitter_ = jitterUv;
  historyValid_ = true;
  ++frameIndex_;
}

void Renderer::cpuMark(const char* name) {
  auto now = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(now - cpuLast_).count();
  cpuLast_ = now;
  for (auto& [n, t] : cpuTotals_)
    if (n == name) { t += ms; return; }
  cpuTotals_.emplace_back(name, ms);
}

std::string Renderer::cpuTimings() {
  if (cpuFrames_ == 0) return {};
  double sum = 0.0;
  for (auto& [n, t] : cpuTotals_) sum += t;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "CPU %.2f ms (%u frames):", sum / cpuFrames_, cpuFrames_);
  std::string out = buf;
  for (auto& [n, t] : cpuTotals_) {
    std::snprintf(buf, sizeof(buf), " %s %.2f |", n, t / cpuFrames_);
    out += buf;
  }
  while (!out.empty() && (out.back() == '|' || out.back() == ' ')) out.pop_back();
  cpuTotals_.clear();
  cpuFrames_ = 0;
  return out;
}

void Renderer::updateDynamicScene(VkCommandBuffer cmd, uint32_t slot) {
  scene::Scene& s = *scene_;
  rhi::Context& ctx = *ctx_;

  // Transformações atual e anterior de cada objeto (vetores de movimento por objeto) e as instâncias do TLAS: só as
  // que mudaram (medido na cidade de 98 mil objetos: ~35 ms de CPU por frame reescrevendo tudo).
  const auto& changed = s.changedObjects();
  for (uint32_t k = 0; k < kFramesInFlight; ++k)
    pendingObjects_[k].insert(pendingObjects_[k].end(), changed.begin(), changed.end());
  if (!historyValid_) objectsFull_.fill(true);  // reinício: anterior = atual em todos os buffers
  auto* objs = static_cast<ObjectGpu*>(objectBuffers_[slot].mapped);
  auto writeObject = [&](uint32_t i) {
    objs[i] = {s.objects[i].transform, historyValid_ ? s.objects[i].prevTransform : s.objects[i].transform};
    if (ctx.rayTracing) s.tlas.writeInstance(slot, i, s.tlasInstance(i));
  };
  const bool full = objectsFull_[slot];
  if (full) {
    for (uint32_t i = 0; i < s.objects.size(); ++i) writeObject(i);
    objectsFull_[slot] = false;
  } else {
    for (uint32_t i : pendingObjects_[slot]) writeObject(i);
  }
  const bool objectsMoved = full || !pendingObjects_[slot].empty();
  pendingObjects_[slot].clear();
  vmaFlushAllocation(ctx.allocator, objectBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);
  cpuMark("objetos");

  // Luzes (podem se mover: faróis, tochas carregadas, etc.) e a BVH de luzes reconstruída para elas — só quando
  // alguma mudou (medido: 35 ms de CPU por frame com 25 mil luzes fixas refeitas à toa).
  if (!s.lights.empty()) {
    const size_t bytes = s.lights.size() * sizeof(scene::PointLight);
    if (lightsSnapshot_.size() != s.lights.size() || std::memcmp(lightsSnapshot_.data(), s.lights.data(), bytes) != 0) {
      lightsSnapshot_ = s.lights;
      buildLightBvh(s.lights, lightBvhNodes_);
      ++lightsVersion_;
    }
    if (slotLightsVersion_[slot] != lightsVersion_) {
      std::memcpy(lightBuffers_[slot].mapped, s.lights.data(), bytes);
      vmaFlushAllocation(ctx.allocator, lightBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);
      std::memcpy(lightBvhBuffers_[slot].mapped, lightBvhNodes_.data(), lightBvhNodes_.size() * sizeof(LightBvhNode));
      vmaFlushAllocation(ctx.allocator, lightBvhBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);
      slotLightsVersion_[slot] = lightsVersion_;
    }
  }
  cpuMark("luzes");

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
  cpuMark("esqueletos");
  // TLAS reconstruído só quando algo mudou (objeto movido ou malha com esqueleto refeita); parado, o da GPU vale.
  if (ctx.rayTracing && (objectsMoved || anySkinned || !tlasBuilt_)) {
    s.tlas.build(cmd, slot, uint32_t(s.objects.size()));
    tlasBuilt_ = true;
  }
  cpuMark("tlas");
}

float Renderer::runAtmosphereTest(uint32_t pathsPerThread) {
  rhi::Context& ctx = *ctx_;
  struct Case { glm::vec4 sun, view; };
  struct Named { float sunElev, viewElev, viewAz; const char* name; };
  std::vector<Named> named;
  for (float sunElev : {60.0f, 20.0f, 5.0f, 1.0f, -2.0f})
    for (auto [ve, va, n] : {std::tuple<float, float, const char*>{90.0f, 0.0f, "zênite"},
                             {30.0f, 0.0f, "30° p/ o sol"}, {30.0f, 180.0f, "30° oposto"},
                             {2.0f, 0.0f, "horizonte p/ sol"}, {2.0f, 180.0f, "horizonte oposto"},
                             {10.0f, 90.0f, "10° lateral"}})
      named.push_back({sunElev, ve, va, n});
  auto dirFrom = [](float elevDeg, float azDeg) {
    float e = glm::radians(elevDeg), a = glm::radians(azDeg);
    return glm::vec4(std::cos(e) * std::cos(a), std::sin(e), std::cos(e) * std::sin(a), 0.0f);
  };
  std::vector<Case> cases;
  for (auto& n : named) cases.push_back({dirFrom(n.sunElev, 0.0f), dirFrom(n.viewElev, n.viewAz)});
  rhi::Buffer caseBuf = rhi::createBufferWithData(ctx, cases.data(), cases.size() * sizeof(Case), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  rhi::Buffer results = rhi::createBuffer(ctx, cases.size() * 4 * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                          rhi::MemoryUsage::GpuToCpu);
  rhi::ComputePass pass;
  pass.init(ctx, "atmosphere_reference.comp", globalLayout_, {SB, SB}, 8);
  uint32_t push[2] = {pathsPerThread, uint32_t(cases.size())};
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    pass.dispatchGroups(cmd, globalSets_[0], {rhi::Binding::storageBuffer(caseBuf), rhi::Binding::storageBuffer(results)},
                        uint32_t(cases.size()) * 3, 1, push);
  });
  vmaInvalidateAllocation(ctx.allocator, results.allocation, 0, VK_WHOLE_SIZE);
  auto* r = static_cast<const glm::vec4*>(results.mapped);
  auto lum = [](glm::vec4 v) { return 0.2126f * v.x + 0.7152f * v.y + 0.0722f * v.z; };
  std::printf("\n%-6s %-18s %12s %12s %12s %8s %8s %9s\n", "sol", "visão", "referência", "motor", "só simples", "erro", "simples", "± ref");
  double sumErr = 0.0;
  int n = 0;
  for (size_t i = 0; i < cases.size(); ++i) {
    float ref = lum(r[i * 4 + 2]), eng = lum(r[i * 4 + 0]), ss = lum(r[i * 4 + 1]), se = lum(r[i * 4 + 3]);
    float err = ref > 0.0f ? (eng - ref) / ref : 0.0f;
    float errSS = ref > 0.0f ? (ss - ref) / ref : 0.0f;
    std::printf("%5.0f° %-18s %12.4e %12.4e %12.4e %+7.1f%% %+7.1f%% %8.1f%%\n", named[i].sunElev, named[i].name, ref, eng, ss,
                100.0f * err, 100.0f * errSS, ref > 0.0f ? 100.0f * se / ref : 0.0f);
    sumErr += std::abs(err);
    ++n;
  }
  float mean = float(sumErr / n);
  std::printf("Erro relativo médio de luminância (motor vs referência): %.2f%%\n", 100.0f * mean);
  pass.destroy(ctx);
  rhi::destroyBuffer(ctx, caseBuf);
  rhi::destroyBuffer(ctx, results);
  return mean;
}

void Renderer::destroy() {
  if (!ctx_) return;
  rhi::Context& ctx = *ctx_;
  vkDeviceWaitIdle(ctx.device);
  destroyTargets();
  profiler_.destroy();
  rhi::destroyImage(ctx, skyLut_);
  for (rhi::Image* img : {&cloudNoise_, &skyClear_, &cloudShadow_}) rhi::destroyImage(ctx, *img);
  vkDestroySampler(ctx.device, cloudSampler_, nullptr);
  rhi::destroyImage(ctx, dfgLut_);
  for (rhi::Image* img : {&transmittanceLut_, &multiScatterLut_, &aerialPerspective_}) rhi::destroyImage(ctx, *img);
  for (VkSampler s : {nearest_, linear_, skySampler_, textureSampler_}) vkDestroySampler(ctx.device, s, nullptr);
  for (auto& [wrap, smp] : wrapSamplers_)
    if (smp != textureSampler_) vkDestroySampler(ctx.device, smp, nullptr);
  wrapSamplers_.clear();
  for (auto* arr : {&frameUbo_, &objectBuffers_, &jointBuffers_, &lightBuffers_, &lightBvhBuffers_})
    for (auto& b : *arr) rhi::destroyBuffer(ctx, b);
  rhi::destroyBuffer(ctx, histogram_);
  rhi::destroyBuffer(ctx, exposureState_);
  rhi::destroyBuffer(ctx, radianceCache_);
  rhi::destroyBuffer(ctx, drawList_);
  for (rhi::ComputePass* p : {&cloudSkyPass_, &cloudShadowPass_, &cloudScreenPass_, &skinningPass_, &aerialPerspectivePass_, &radianceCacheResolvePass_, &skyLutPass_, &restirTemporalPass_, &restirSpatialPass_, &lightingPass_, &svgfTemporalPass_,
                              &svgfAtrousPass_, &compositePass_, &taaPass_, &rrGuidesPass_, &accumulatePass_, &histogramPass_, &exposurePass_, &dofPass_,
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
