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
constexpr uint32_t kFogSlices = 64;  // fatias da névoa volumétrica (células de 8x8 pixels)
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
  glm::vec4 weather;      // x = superfícies molhadas (0..1), y = poças (0..1), z = chuva caindo (respingos),
                          // w = tempo da cena no frame anterior (movimento de vento/ondas para os vetores de movimento)
  glm::mat4 shadowMatrices[4];  // shadow maps em cascata: mundo -> clip de cada cascata
  glm::vec4 shadowSplits;       // fim de cada cascata (m); w = 0: desligado
  glm::vec4 shadowParams;       // x = m por texel da cascata 0, y = 1: shadow maps mesmo com ray tracing
  glm::vec4 water;              // x = nível do mar (m), y = 1: há oceano (vista subaquática e cáusticas)
  glm::vec4 ripple;             // ondulações: xy = canto da grade (m), z = tamanho (m), w = 1 ligadas
};
static_assert(sizeof(FrameUniforms) == 992);
constexpr uint32_t kRippleSize = 256, kMaxDisturbers = 64;
constexpr float kRippleCell = 0.25f;  // m por texel: grade de 64 m
constexpr uint32_t kShadowMapSize = 2048;

// Frente dos triângulos: anti-horário, como no glTF (a projeção inverte o Y e o viewport do Vulkan também: as duas
// inversões se cancelam — conferido pela imagem, com culling o chão e as paredes continuam lá).
constexpr bool kFrontFaceClockwise = false;

// Espelha CullBatch em shaders/cull.comp.
struct CullBatch {
  glm::vec3 boundsMin;
  uint32_t first;
  glm::vec3 boundsMax;
  uint32_t alwaysVisible;
  float lodNear, lodFar;  // distância (m) da câmera ao objeto em que este lote desenha
  uint32_t lod;
  float worldMargin;  // folga em metros do mundo (vento da folhagem), somada à caixa já dividida pela escala do objeto
};

// Constantes do G-buffer, as mesmas para os dois caminhos (um layout só): gbuffer.vert lê só drawList; gbuffer.task
// e gbuffer.mesh leem tudo (== Push em shaders/meshlet.glsl).
struct GBufferPush {
  VkDeviceAddress drawList;  // lista de objetos ordenada por malha (shaders/gbuffer.vert)
  VkDeviceAddress commands, meshlets, vertices;
  uint32_t batch, meshletCount, vertexBase, triangleBase;
  uint32_t flags, pad;
};
// Espelha ObjectData em shaders/scene.glsl.
struct ObjectGpu {
  glm::mat4 model, prevModel;
  glm::uvec4 anim;  // multidão: x = 1ª posição do clipe em crowdFrames (~0 = comum), y = pose atual, z = anterior
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
struct ParticlePush {
  uint32_t emitterCount, rayTracing;
  float time;
  uint32_t pad;
};
struct TonemapPush {
  float bloomStrength, bloomNorm;
  uint32_t manualSrgb;
  uint32_t debugView;  // 1 = mostra o valor cru (linear) sem exposição, bloom nem tonemap
  float sharpness;     // RCAS
  float lensFlare;     // intensidade do reflexo nas lentes (0 = desligado)
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
  rainOcclusion_ = rhi::createImage2D(ctx, {256, 256}, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
  useShadowMaps_ = !ctx.rayTracing || settings.shadowMaps;
  {
    const uint32_t size = useShadowMaps_ ? kShadowMapSize : 16;  // sem uso: só um alvo válido para o descritor
    shadowMap_ = rhi::createImage2DArray(ctx, {size, size}, 4, kDepthFormat,
                                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    for (uint32_t c = 0; c < 4; ++c) shadowLayerViews_[c] = rhi::createLayerView(ctx, shadowMap_, c);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VK_CHECK(vkCreateSampler(ctx.device, &si, nullptr, &shadowSampler_));
  }
  cloudSampler_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
  oceanSampler_ = makeSampler(ctx, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT);
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
    // Os misturados (decals BLEND) vão por último: são desenhados num segundo passe, por cima.
    auto key = [&](uint32_t i) {
      const auto& o = scene.objects[i];
      const uint32_t flags = scene.materials[o.material].flags;
      const bool ds = (flags & scene::kMaterialDoubleSided) != 0, blend = (flags & scene::kMaterialBlend) != 0;
      const bool mirrored = glm::determinant(glm::mat3(o.transform)) < 0.0f;
      return std::make_tuple(blend, ds, mirrored, o.mesh);
    };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
    for (uint32_t k = 0; k < order.size(); ++k) {
      const auto [blend, ds, mirrored, mesh] = key(order[k]);
      if (drawBatches_.empty() || drawBatches_.back().mesh != mesh || drawBatches_.back().doubleSided != ds ||
          drawBatches_.back().mirrored != mirrored || drawBatches_.back().blend != blend) {
        drawBatches_.push_back({mesh, k, 0, ds, mirrored});
        drawBatches_.back().blend = blend;
      }
      ++drawBatches_.back().count;
    }
    // Culling na GPU (cull.comp): lote de cada entrada, caixa local da malha por lote e um comando de desenho
    // indireto por lote (o compute preenche o número de instâncias visíveis).
    // LODs: cada lote de malha com níveis simplificados ganha lotes extras com as mesmas entradas e a malha do nível;
    // o culling escolhe, por distância, qual dos lotes desenha cada objeto.
    const size_t baseBatches = drawBatches_.size();
    for (size_t b = 0; b < baseBatches; ++b) {
      const DrawBatch base = drawBatches_[b];
      const scene::GpuMesh& gm = scene.meshes[base.mesh];
      for (int k = 0; k < 2; ++k) {
        if (gm.lod[k] < 0) continue;
        DrawBatch l = base;
        l.mesh = uint32_t(gm.lod[k]);
        l.first = uint32_t(order.size());
        l.lod = uint32_t(k + 1);
        l.lodBase = uint32_t(b);
        // Cópia antes: inserir um trecho do próprio vetor nele mesmo invalida os iteradores quando ele cresce.
        const std::vector<uint32_t> entries(order.begin() + base.first, order.begin() + base.first + base.count);
        order.insert(order.end(), entries.begin(), entries.end());
        drawBatches_.push_back(l);
      }
    }
    std::vector<uint32_t> entryBatch(order.size(), 0);
    std::vector<CullBatch> cullBatches;
    std::vector<VkDrawIndexedIndirectCommand> commands;
    std::vector<uint32_t> meshletGroups;
    for (uint32_t b = 0; b < drawBatches_.size(); ++b) {
      const DrawBatch& db = drawBatches_[b];
      const scene::GpuMesh& gm = scene.meshes[db.mesh];
      bool always = gm.deformable(), foliage = false, crowd = false, special = false;
      for (uint32_t k = db.first; k < db.first + db.count; ++k) {
        entryBatch[k] = b;
        const uint32_t flags = scene.materials[scene.objects[order[k]].material].flags;
        always = always || (flags & scene::kMaterialWater);  // o mar segue a câmera no vertex shader
        foliage = foliage || (flags & scene::kMaterialFoliage);
        crowd = crowd || scene.objects[order[k]].crowdClip != ~0u;
        special = special || (flags & (scene::kMaterialWater | scene::kMaterialFoliage | scene::kMaterialHair));
      }
      // Meshlets só no caminho rígido de gbuffer.vert (sem vento, cabelo, mar, multidão, esqueleto nem tecido) e em
      // malhas grandes o bastante (≥ 8 meshlets, ~1.000 triângulos) para o descarte por pedaço pagar o grupo do task
      // shader por instância: medido no llvmpipe, malhas pequenas repetidas (cidade, mundo) ficavam mais lentas.
      drawBatches_[b].hlodProxy = scene.objects[order[db.first]].hlodProxy;
      drawBatches_[b].meshlets = gm.meshletCount >= 8 && !gm.deformable() && !special && !crowd && !db.blend;
      meshletGroups.push_back(drawBatches_[b].meshlets ? (gm.meshletCount + 31) / 32 : 0);
      // As poses da multidão saem da caixa da pose 0 (em unidades da malha). O vento empurra a folhagem até ~1,1 m para
      // fora da caixa, em metros do mundo (gbuffer.vert: curvatura máx. 0,02·12^1,4·1,6 + tremor): essa folga vai à
      // parte, porque a escala do objeto varia (no Sponza, 0,008: 0,6 "da malha" seriam 5 mm e a oclusão escondia vasos).
      const glm::vec3 margin(crowd ? 0.4f : 0.0f);
      const float worldMargin = foliage ? 1.1f : 0.0f;
      // Faixa de distância deste nível (a malha base dá a caixa e as distâncias de troca).
      const scene::GpuMesh& baseMesh = db.lod == 0 ? gm : scene.meshes[drawBatches_[db.lodBase].mesh];
      float lodNear = 0.0f, lodFar = 1e30f;
      if (baseMesh.lod[0] >= 0) {
        lodNear = db.lod == 0 ? 0.0f : baseMesh.lodDistance[db.lod - 1];
        lodFar = db.lod == 0 ? baseMesh.lodDistance[0] : db.lod == 1 ? baseMesh.lodDistance[1] : 1e30f;
      }
      cullBatches.push_back({baseMesh.boundsMin - margin, db.first, baseMesh.boundsMax + margin, always ? 1u : 0u, lodNear,
                             lodFar, db.lod, worldMargin});
      commands.push_back({gm.indexCount, 0, 0, 0, db.first});
    }
    if (order.empty()) order.push_back(0);
    if (cullBatches.empty()) {
      cullBatches.push_back({});
      commands.push_back({});
    }
    meshletGroups.resize(commands.size(), 0);
    std::vector<uint32_t> hlodInfo(std::max<size_t>(scene.objects.size(), 1), 0);
    for (size_t i = 0; i < scene.objects.size(); ++i)
      if (scene.objects[i].hlodCell >= 0) hlodInfo[i] = uint32_t(scene.objects[i].hlodCell + 1) | (scene.objects[i].hlodProxy ? 0x80000000u : 0u);
    std::vector<glm::vec4> cells;
    for (const scene::HlodCell& c : scene.hlodCells) {
      cells.push_back(glm::vec4(c.lo, c.distance));
      cells.push_back(glm::vec4(c.hi, 0.0f));
    }
    if (cells.empty()) cells.resize(2, glm::vec4(0.0f));
    hlodInfo_ = rhi::createBufferWithData(ctx, hlodInfo.data(), hlodInfo.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    hlodCells_ = rhi::createBufferWithData(ctx, cells.data(), cells.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    meshletGroups_ = rhi::createBufferWithData(ctx, meshletGroups.data(), meshletGroups.size() * sizeof(uint32_t),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    for (rhi::Buffer& a : meshletArgs_)
      a = rhi::createBuffer(ctx, commands.size() * 3 * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                            rhi::MemoryUsage::GpuOnly);
    // cull.comp guarda o objeto em 24 bits da entrada (o resto: pontilhado da troca e nível de detalhe).
    if (scene.objects.size() >= (1u << 24)) FATAL("Cena com %zu objetos (máximo 16.777.215)", scene.objects.size());
    drawEntries_ = uint32_t(scene.objects.empty() ? 0 : order.size());
    drawList_ = rhi::createBufferWithData(ctx, order.data(), order.size() * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    entryBatch_ = rhi::createBufferWithData(ctx, entryBatch.data(), entryBatch.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    cullBatches_ = rhi::createBufferWithData(ctx, cullBatches.data(), cullBatches.size() * sizeof(CullBatch), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    drawCommands_ = rhi::createBufferWithData(ctx, commands.data(), commands.size() * sizeof(VkDrawIndexedIndirectCommand),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    culledList_ = rhi::createBuffer(ctx, order.size() * sizeof(uint32_t),
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    rhi::MemoryUsage::GpuOnly);
    culledList2_ = rhi::createBuffer(ctx, order.size() * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     rhi::MemoryUsage::GpuOnly);
    drawCommands2_ = rhi::createBufferWithData(ctx, commands.data(), commands.size() * sizeof(VkDrawIndexedIndirectCommand),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
    const std::vector<uint32_t> none(order.size(), 0);
    visibleLast_ = rhi::createBufferWithData(ctx, none.data(), none.size() * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
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
      {17, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {18, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {19, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {20, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {21, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {22, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {23, CIS, 1, VK_SHADER_STAGE_ALL, nullptr},
      {24, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
      {25, SB, 1, VK_SHADER_STAGE_ALL, nullptr},
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
      {CIS, kFramesInFlight * (12 + kMaxTextures)},
      {SB, kFramesInFlight * 11},
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
  if (scene.ocean) ocean_.init(ctx, globalLayout_, scene.windSpeed, scene.windDirection, scene.waveHeight);
  // Ondulações: ligadas se a cena tem água (mar, ou rio: o nível é a altura da malha dele, que é plana).
  rippleOn_ = scene.ocean && settings.ripples;
  rippleLevel_ = scene.seaLevel;
  for (const auto& o : scene.objects)
    if (!rippleOn_ && settings.ripples && (scene.materials[o.material].flags & scene::kMaterialRiver)) {
      const scene::GpuMesh& gm = scene.meshes[o.mesh];
      rippleLevel_ = (o.transform * glm::vec4(0.5f * (gm.boundsMin + gm.boundsMax), 1.0f)).y;
      rippleOn_ = true;
    }
  for (rhi::Image& img : ripple_)
    img = rhi::createImage2D(ctx, {kRippleSize, kRippleSize}, VK_FORMAT_R32G32_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    for (rhi::Image& img : ripple_) {
      rhi::imageBarrier(cmd, img.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
      VkClearColorValue zero{};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, img.handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
  });
  terrainLayers_ = rhi::createBufferWithData(ctx, &scene.terrainLayers, sizeof(scene::TerrainLayers), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  for (rhi::Buffer& b : textureFeedback_) {
    b = rhi::createBuffer(ctx, 2 * kMaxTextures * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
    std::memset(b.mapped, 0, 2 * kMaxTextures * sizeof(uint32_t));
  }
  for (rhi::Buffer& b : rippleDisturbers_)
    b = rhi::createBuffer(ctx, kMaxDisturbers * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, rhi::MemoryUsage::CpuToGpu);
  ripplePass_.init(ctx, "ripple.comp", globalLayout_, {CIS, SI, SB}, 32);
  {  // multidões: endereço dos vértices de cada pose assada
    std::vector<VkDeviceAddress> addresses;
    for (const scene::CrowdClip& c : scene.crowdClips) {
      crowdFrameBase_.push_back(uint32_t(addresses.size()));
      // Poses do nível 0, depois as dos níveis 1 e 2 (gbuffer.vert: base + nível × poses + pose).
      for (uint32_t level = 0; level < 3; ++level) {
        const uint32_t first = level == 0 || c.lodFirstMesh[level - 1] == ~0u ? c.firstMesh : c.lodFirstMesh[level - 1];
        for (uint32_t k = 0; k < c.frames; ++k) addresses.push_back(scene.meshes[first + k].vertices.address);
      }
    }
    if (addresses.empty()) addresses.push_back(0);
    crowdFrames_ = rhi::createBufferWithData(ctx, addresses.data(), addresses.size() * sizeof(VkDeviceAddress),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
  writeGlobalDescriptors();

  // ---- Passes ----
  VkDescriptorSetLayout g = globalLayout_;
  const bool rt = ctx.rayTracing;
  skinningPass_.init(ctx, "skinning.comp", g, {}, sizeof(SkinningPush));
  clothPass_.init(ctx, "cloth.comp", g, {}, 112);
  hairSimPass_.init(ctx, "hair_sim.comp", g, {}, 56);
  skyLutPass_.init(ctx, "skyview.comp", g, {SI});
  aerialPerspectivePass_.init(ctx, "aerial_perspective.comp", g, {SI});
  cloudSkyPass_.init(ctx, "clouds_sky.comp", g, {SI, CIS});
  cloudShadowPass_.init(ctx, "cloud_shadow.comp", g, {SI});
  cloudScreenPass_.init(ctx, "clouds_screen.comp", g, {SI, CIS, CIS});
  if (settings.cloudReference) cloudReferencePass_.init(ctx, "clouds_reference.comp", g, {SI, CIS, CIS}, sizeof(uint32_t));
  fogInjectPass_.init(ctx, rt ? "fog_inject_rt.comp" : "fog_inject.comp", g, {SI, CIS});
  fogIntegratePass_.init(ctx, "fog_integrate.comp", g, {CIS, SI});
  cullPass_.init(ctx, "cull.comp", g, {SB, SB, SB, SB, SB, SB, CIS, SB, SB}, 5 * sizeof(uint32_t));
  hiZBuildPass_.init(ctx, "hiz_build.comp", g, {CIS, SI}, sizeof(uint32_t));
  meshletArgsPass_.init(ctx, "meshlet_args.comp", g, {SB, SB, SB}, 2 * sizeof(uint32_t));
  cullResetPass_.init(ctx, "cull_reset.comp", g, {SB}, sizeof(uint32_t));
  sssPass_.init(ctx, "sss.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, SI}, sizeof(glm::ivec2));
  if (rt) rainOcclusionPass_.init(ctx, "rain_occlusion.comp", g, {SI});
  for (const auto& m : scene.materials) hasSubsurface_ = hasSubsurface_ || (m.flags & scene::kMaterialSubsurface);
  restirTemporalPass_.init(ctx, rt ? "restir_temporal_rt.comp" : "restir_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, SB, SB, CIS, CIS});
  restirSpatialPass_.init(ctx, rt ? "restir_spatial_rt.comp" : "restir_spatial.comp", g, {CIS, CIS, CIS, CIS, SB, SB, CIS});
  lightingPass_.init(ctx, rt ? "lighting_rt.comp" : "lighting.comp", g, {CIS, CIS, CIS, CIS, SB, SI, SI, CIS, SI, SI});
  svgfTemporalPass_.init(ctx, "svgf_temporal.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, CIS, SI, SI, SI, SI});
  svgfAtrousPass_.init(ctx, "svgf_atrous.comp", g, {CIS, CIS, CIS, CIS, CIS, SI}, sizeof(AtrousPush));
  compositePass_.init(ctx, "composite.comp", g, {CIS, CIS, CIS, CIS, CIS, CIS, CIS, SI, CIS, CIS, CIS});
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
  if (ctx.meshShaders) {  // mesmo fragment shader, mesmas saídas; task + mesh no lugar do vertex shader
    rhi::GraphicsPipelineDesc md = gb;
    md.taskShader = "gbuffer.task";
    md.meshShader = "gbuffer.mesh";
    md.vertexBindings.clear();
    md.vertexAttributes.clear();
    gbufferMeshPipeline_ = rhi::createGraphicsPipeline(ctx, md);
  }
  {  // decals BLEND: sem escrever profundidade; mistura por alfa na cor difusa, normal e f0 (o .a de cada anexo,
     // que guarda outra coisa — oclusão, rugosidade, f90 —, fica como estava); movimento, emissão e visibilidade não
    rhi::GraphicsPipelineDesc bd = gb;
    bd.depthWrite = false;
    VkPipelineColorBlendAttachmentState mix{};
    mix.blendEnable = VK_TRUE;
    mix.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    mix.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    mix.colorBlendOp = VK_BLEND_OP_ADD;
    mix.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mix.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    mix.alphaBlendOp = VK_BLEND_OP_ADD;
    mix.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendAttachmentState none{};
    bd.blendStates = {mix, mix, none, none, mix, none};  // diffuse, normal, motion, emissive, specular, visibility
    gbufferBlendPipeline_ = rhi::createGraphicsPipeline(ctx, bd);
  }
  if (useShadowMaps_) {  // mesma entrada de vértices do G-buffer, só profundidade (convencional: 0 perto, 1 longe)
    shadowLayout_.init(ctx, g, {}, 16);
    rhi::GraphicsPipelineDesc sd = gb;
    sd.vertexShader = "shadow.vert";
    sd.fragmentShader = "shadow.frag";
    sd.layout = shadowLayout_.pipelineLayout;
    sd.colorFormats = {};
    sd.depthCompare = VK_COMPARE_OP_LESS_OR_EQUAL;
    sd.dynamicCulling = false;
    sd.cullMode = VK_CULL_MODE_NONE;  // as duas faces projetam sombra (sem vazamento em paredes finas)
    shadowPipeline_ = rhi::createGraphicsPipeline(ctx, sd);
  }

  // Partículas: quadrados sobre a imagem composta, antes do TAA (mistura pré-multiplicada, sem depth: o fragment
  // compara com a profundidade da cena).
  particleLayout_.init(ctx, g, {SB, CIS}, sizeof(ParticlePush));
  if (!scene.emitters.empty()) {
    emitterBuffer_ = rhi::createBufferWithData(ctx, scene.emitters.data(), scene.emitters.size() * sizeof(scene::ParticleEmitter),
                                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    rhi::GraphicsPipelineDesc pd;
    pd.vertexShader = ctx.rayTracing ? "particles_rt.vert" : "particles.vert";
    pd.fragmentShader = "particles.frag";
    pd.layout = particleLayout_.pipelineLayout;
    pd.colorFormats = {kHdrFormat};
    pd.blendPremultiplied = true;
    particlePipeline_ = rhi::createGraphicsPipeline(ctx, pd);
  }

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
                            &skyClear_, &cloudShadow_, &rainOcclusion_})
      rhi::imageBarrier(cmd, img->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    rhi::imageBarrier(cmd, shadowMap_.handle, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
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

std::vector<VkDescriptorImageInfo> Renderer::textureInfos() {
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
  return texInfos;
}

void Renderer::writeGlobalDescriptors() {
  scene::Scene& s = *scene_;
  const std::vector<VkDescriptorImageInfo> texInfos = textureInfos();
  for (uint32_t i = 0; i < kFramesInFlight; ++i) texturesVersionWritten_[i] = s.texturesVersion;

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
    // Oceano (sem oceano na cena: qualquer imagem válida; os shaders só leem com material de água).
    VkDescriptorImageInfo ocean[2] = {{oceanSampler_, ocean_.ready() ? ocean_.displacement().view : cloudShadow_.view, VK_IMAGE_LAYOUT_GENERAL},
                                      {oceanSampler_, ocean_.ready() ? ocean_.slope().view : cloudShadow_.view, VK_IMAGE_LAYOUT_GENERAL}};
    for (uint32_t b = 0; b < 2; ++b) add(17 + b, CIS, 1).pImageInfo = &ocean[b];
    VkDescriptorBufferInfo decals{s.decalBuffer.handle, 0, VK_WHOLE_SIZE};
    add(19, SB, 1).pBufferInfo = &decals;
    VkDescriptorImageInfo rainOcc{linear_, rainOcclusion_.view, VK_IMAGE_LAYOUT_GENERAL};
    add(20, CIS, 1).pImageInfo = &rainOcc;
    VkDescriptorImageInfo cascades{shadowSampler_, shadowMap_.view, VK_IMAGE_LAYOUT_GENERAL};
    add(21, CIS, 1).pImageInfo = &cascades;
    VkDescriptorBufferInfo crowd{crowdFrames_.handle, 0, VK_WHOLE_SIZE};
    add(22, SB, 1).pBufferInfo = &crowd;
    VkDescriptorImageInfo rippleInfo{linear_, ripple_[0].view, VK_IMAGE_LAYOUT_GENERAL};
    add(23, CIS, 1).pImageInfo = &rippleInfo;
    VkDescriptorBufferInfo feedback{textureFeedback_[i].handle, 0, VK_WHOLE_SIZE};
    add(24, SB, 1).pBufferInfo = &feedback;
    VkDescriptorBufferInfo terrain{terrainLayers_.handle, 0, VK_WHOLE_SIZE};
    add(25, SB, 1).pBufferInfo = &terrain;
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
  t_.sceneColor = rhi::createImage2D(ctx, R, kHdrFormat, storage | upscalerIo | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
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
  const VkExtent3D froxels{(R.width + 7) / 8, (R.height + 7) / 8, kFogSlices};
  for (auto& f : t_.fogInject) f = rhi::createImage3D(ctx, froxels, kHdrFormat, storage);
  t_.fogIntegrated = rhi::createImage3D(ctx, froxels, kHdrFormat, storage);
  hiZ_ = rhi::createImage2D(ctx, R, VK_FORMAT_R32_SFLOAT, storage, rhi::mipCount(R));
  for (uint32_t m = 0; m < hiZ_.mipLevels; ++m) hiZMips_.push_back(rhi::createMipView(ctx, hiZ_, m));
  t_.sssA = rhi::createImage2D(ctx, R, kHdrFormat, storage);
  t_.sssB = rhi::createImage2D(ctx, R, kHdrFormat, storage);
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
                            &t_.clouds[1], &t_.fogInject[0], &t_.fogInject[1], &t_.fogIntegrated, &t_.sssA, &t_.sssB, &hiZ_})
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
                            &t_.clouds[1], &t_.fogInject[0], &t_.fogInject[1], &t_.fogIntegrated, &t_.sssA, &t_.sssB})
    rhi::destroyImage(ctx, *img);
  for (VkImageView v : t_.bloomMips) vkDestroyImageView(ctx.device, v, nullptr);
  t_.bloomMips.clear();
  for (VkImageView v : hiZMips_) vkDestroyImageView(ctx.device, v, nullptr);
  hiZMips_.clear();
  rhi::destroyImage(ctx, hiZ_);
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
  // Streaming de texturas: o que a tela pediu no último frame deste slot (a GPU já terminou com ele) decide os mips
  // que sobem ou descem; depois os descritores deste slot apontam para as imagens novas (as antigas esperam os
  // frames em voo no cemitério da cena).
  {
    rhi::Context& c = *ctx_;
    auto* need = static_cast<uint32_t*>(textureFeedback_[slot].mapped);
    vmaInvalidateAllocation(c.allocator, textureFeedback_[slot].allocation, 0, VK_WHOLE_SIZE);
    if (textureFeedbackValid_[slot]) scene_->updateTextureStreaming(c, need, uint32_t(scene_->textures.size()));
    std::memset(need, 0, kMaxTextures * sizeof(uint32_t));
    for (size_t i = 0; i < scene_->textures.size(); ++i)  // nível de cada textura que é o nível 0 da imagem
      need[kMaxTextures + i] = i < scene_->textureStreams.size() ? scene_->textureStreams[i].base : 0;
    vmaFlushAllocation(c.allocator, textureFeedback_[slot].allocation, 0, VK_WHOLE_SIZE);
    textureFeedbackValid_[slot] = true;
    if (texturesVersionWritten_[slot] != scene_->texturesVersion && !scene_->textures.empty()) {
      const std::vector<VkDescriptorImageInfo> infos = textureInfos();
      VkWriteDescriptorSet x{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      x.dstSet = gs;
      x.dstBinding = 5;
      x.descriptorCount = uint32_t(infos.size());
      x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      x.pImageInfo = infos.data();
      vkUpdateDescriptorSets(c.device, 1, &x, 0, nullptr);
      texturesVersionWritten_[slot] = scene_->texturesVersion;
    }
  }
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
  const bool rainMap = ctx_->rayTracing && scene_->wetness > 0.0f;  // mapa de oclusão da chuva (rain_occlusion.comp)
  u.weather = glm::vec4(scene_->wetness, scene_->puddles, rainMap ? scene_->rain : -1.0f - scene_->rain,
                        historyValid_ ? prevSceneTime_ : scene_->time);
  prevSceneTime_ = scene_->time;
  u.water = glm::vec4(scene_->seaLevel, scene_->ocean ? 1.0f : 0.0f, 0.0f, 0.0f);
  // Ondulações: a grade anda com a câmera em passos de um texel (só com a cena andando: parada, o estado fica).
  rippleDt_ = rippleValid_ ? std::max(scene_->time - rippleTime_, 0.0f) : 0.0f;
  rippleTime_ = scene_->time;
  rippleShift_ = glm::ivec2(0);
  if (rippleOn_ && (rippleDt_ > 0.0f || !rippleValid_)) {
    const glm::ivec2 cell = glm::ivec2(glm::floor(glm::vec2(cam.position.x, cam.position.z) / kRippleCell)) - int(kRippleSize / 2);
    rippleShift_ = rippleValid_ ? cell - rippleCell_ : glm::ivec2(0);
    rippleCell_ = cell;
  }
  rippleValid_ = rippleOn_;
  u.ripple = rippleOn_ ? glm::vec4(glm::vec2(rippleCell_) * kRippleCell, float(kRippleSize) * kRippleCell, 1.0f) : glm::vec4(0.0f);
  if (scene_->sunIlluminance <= 0.0f) {
    u.night.y = 2.5e-6f;
    if (scene_->moonIlluminance > 0.0f) {
      u.sunDir = glm::vec4(glm::normalize(scene_->moonDirection), scene_->moonIlluminance);
      u.night.x = 1.0f;
    }
  }
  u.params = glm::vec4(settings.sunAngularRadius, dt, scene_->fogDensity, scene_->fogFalloff);
  u.jitter = glm::vec4(jitterUv, prevJitter_);
  // Shadow maps em cascata: 4 fatias do frustum (divisão "prática", λ = 0,75: meio caminho entre logarítmica e
  // uniforme), cada uma numa projeção ortográfica do sol que cobre a esfera envolvente da fatia (não muda de tamanho ao
  // girar a câmera) e anda em passos de um texel (a sombra não "tremula" quando a câmera anda).
  if (useShadowMaps_ && u.sunDir.w > 0.0f) {
    const glm::vec3 L = glm::vec3(u.sunDir), fwd = cam.forward(), right = cam.right(), up = glm::cross(right, fwd);
    const float n = cam.nearPlane, f = settings.shadowDistance, th = std::tan(0.5f * cam.fovY);
    float prevSplit = n;
    for (uint32_t c = 0; c < 4; ++c) {
      const float t = float(c + 1) / 4.0f;
      const float split = 0.75f * n * std::pow(f / n, t) + 0.25f * (n + (f - n) * t);
      glm::vec3 corners[8], center(0.0f);
      for (int k = 0; k < 8; ++k) {
        const float d = (k & 4) ? split : prevSplit, hh = d * th, ww = hh * aspect;
        corners[k] = cam.position + fwd * d + right * ((k & 1) ? ww : -ww) + up * ((k & 2) ? hh : -hh);
        center += corners[k] / 8.0f;
      }
      float radius = 0.0f;
      for (const glm::vec3& p : corners) radius = std::max(radius, glm::length(p - center));
      radius = std::ceil(radius * 16.0f) / 16.0f;
      const float back = 400.0f;  // quem fica atrás (até 400 m na direção do sol) também projeta sombra
      const glm::vec3 lup = std::abs(L.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
      const glm::mat4 view = glm::lookAt(center + L * (radius + back), center, lup);
      glm::mat4 proj = glm::orthoRH_ZO(-radius, radius, -radius, radius, 0.0f, 2.0f * radius + back);
      glm::vec4 o = proj * view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
      const float half = 0.5f * float(kShadowMapSize);
      const glm::vec2 snapped = glm::round(glm::vec2(o) * half) / half;
      proj[3][0] += snapped.x - o.x;
      proj[3][1] += snapped.y - o.y;
      shadowMatrices_[c] = proj * view;
      u.shadowMatrices[c] = shadowMatrices_[c];
      if (c < 3) u.shadowSplits[c] = split;
      if (c == 0) u.shadowParams.x = 2.0f * radius / float(kShadowMapSize);
      prevSplit = split;
    }
    u.shadowSplits.w = prevSplit;
    u.shadowParams.y = (ctx_->rayTracing && settings.shadowMaps) ? 1.0f : 0.0f;
  } else {
    u.shadowSplits = glm::vec4(0.0f);
    u.shadowParams = glm::vec4(0.0f);
  }
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
  if (ocean_.ready()) ocean_.update(cmd, gs, scene_->time);
  if (rippleOn_ && rippleDt_ > 0.0f) {
    // Perturbadores: objetos que andaram e cruzam o nível da água (caixa do mundo), velocidade pelo deslocamento.
    auto* dist = static_cast<glm::vec4*>(rippleDisturbers_[slot].mapped);
    uint32_t count = 0;
    for (const auto& o : scene_->objects) {
      if (count == kMaxDisturbers) break;
      const glm::vec3 p = glm::vec3(o.transform[3]), q = glm::vec3(o.prevTransform[3]);
      const float speed = glm::length(glm::vec2(p.x - q.x, p.z - q.z)) / rippleDt_;
      if (speed < 0.05f) continue;
      const scene::GpuMesh& gm = scene_->meshes[o.mesh];
      glm::vec3 lo(1e30f), hi(-1e30f);
      for (int k = 0; k < 8; ++k) {
        const glm::vec3 c((k & 1) ? gm.boundsMax.x : gm.boundsMin.x, (k & 2) ? gm.boundsMax.y : gm.boundsMin.y,
                          (k & 4) ? gm.boundsMax.z : gm.boundsMin.z);
        const glm::vec3 w = glm::vec3(o.transform * glm::vec4(c, 1.0f));
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
      }
      if (lo.y > rippleLevel_ || hi.y < rippleLevel_) continue;
      dist[count++] = glm::vec4(0.5f * (lo.x + hi.x), 0.5f * (lo.z + hi.z), 0.5f * std::min(hi.x - lo.x, hi.z - lo.z), speed);
    }
    // Dois subpassos: 0 → 1 (com o deslocamento da grade) e 1 → 0 (o resultado fica sempre em ripple_[0]).
    struct {
      glm::ivec2 shift;
      float dt;
      uint32_t count;
      glm::vec2 origin;
      float cell, pad;
    } rp{rippleShift_, 0.5f * std::min(rippleDt_, 0.1f), count, glm::vec2(rippleCell_) * kRippleCell, kRippleCell, 0.0f};
    for (int step = 0; step < 2; ++step) {
      ripplePass_.dispatch(cmd, gs, {Binding::sampled(ripple_[step].view, nearest_), Binding::storageImage(ripple_[1 - step].view),
                                     Binding::storageBuffer(rippleDisturbers_[slot])}, {kRippleSize, kRippleSize}, &rp);
      barrier();
      rp.shift = glm::ivec2(0);
    }
  }
  barrier();
  profiler_.mark(cmd, "cena");

  // ---- 1. Atmosfera (e nuvens: o céu limpo vai para skyClear_ e as nuvens o compõem na LUT final) ----
  const bool clouds = scene_->cloudCoverage > 0.0f && scene_->uniformSkyLuminance <= 0.0f;
  skyLutPass_.dispatch(cmd, gs, {Binding::storageImage(clouds ? skyClear_.view : skyLut_.view)}, skyLutExtent_);
  aerialPerspectivePass_.dispatch(cmd, gs, {Binding::storageImage(aerialPerspective_.view)},
                                  {kAerialPerspectiveExtent.width, kAerialPerspectiveExtent.height});
  if (clouds) cloudShadowPass_.dispatch(cmd, gs, {Binding::storageImage(cloudShadow_.view)}, kCloudShadowExtent);
  if (ctx_->rayTracing && scene_->wetness > 0.0f)
    rainOcclusionPass_.dispatch(cmd, gs, {Binding::storageImage(rainOcclusion_.view)}, {256, 256});
  barrier();
  if (clouds) {
    cloudSkyPass_.dispatch(cmd, gs, {Binding::storageImage(skyLut_.view), Binding::sampled(skyClear_.view, linear_)},
                           skyLutExtent_);
    if (settings.cloudReference) {
      cloudReferencePass_.dispatch(cmd, gs, {Binding::storageImage(t_.clouds[cur].view), Binding::sampled(t_.clouds[cur ^ 1u].view, nearest_),
                                             Binding::sampled(skyClear_.view, linear_)},
                                   t_.clouds[cur].extent, &cloudReferenceFrames_);
      ++cloudReferenceFrames_;
    } else {
      cloudScreenPass_.dispatch(cmd, gs, {Binding::storageImage(t_.clouds[cur].view), Binding::sampled(t_.clouds[cur ^ 1u].view, linear_),
                                          Binding::sampled(skyClear_.view, linear_)},
                                t_.clouds[cur].extent);
    }
    barrier();
  }
  // Névoa volumétrica: luz do sol com sombra em cada célula, depois a soma ao longo das fatias.
  if (scene_->fogDensity > 0.0f) {
    const VkExtent3D fe{t_.fogInject[cur].extent.width, t_.fogInject[cur].extent.height, t_.fogInject[cur].depth};
    fogInjectPass_.dispatch(cmd, gs, {Binding::storageImage(t_.fogInject[cur].view), Binding::sampled(t_.fogInject[cur ^ 1u].view, linear_)},
                            {fe.width, fe.height * fe.depth});
    barrier();
    fogIntegratePass_.dispatch(cmd, gs, {Binding::sampled(t_.fogInject[cur].view, linear_), Binding::storageImage(t_.fogIntegrated.view)},
                               {fe.width, fe.height});
    barrier();
  }
  profiler_.mark(cmd, "ceu");

  // ---- 2. G-buffer: culling na GPU (frustum + oclusão em duas fases) e desenho indireto ----
  // Fase 0: desenha o que estava visível no frame anterior. Monta a pirâmide de profundidade (HiZ) dessa passada.
  // Fase 1: testa tudo contra ela; desenha (por cima, carregando o G-buffer) o que apareceu. Sem oclusão desligada.
  {
    const uint32_t batchCount = uint32_t(drawBatches_.size()), entries = drawEntries_;
    const bool occlusion = settings.gpuCulling && settings.occlusionCulling && historyValid_;
    const bool meshlets = settings.meshlets && gbufferMeshPipeline_ != VK_NULL_HANDLE;
    auto cull = [&](uint32_t phase, const rhi::Buffer& commands, const rhi::Buffer& culled) {
      cullResetPass_.dispatchGroups(cmd, gs, {Binding::storageBuffer(commands)}, (batchCount + 63) / 64 + 1, 1, &batchCount);
      barrier();
      const uint32_t cp[5] = {entries, settings.gpuCulling ? 1u : 0u, phase, occlusion ? 1u : 0u, settings.hlod ? 1u : 0u};
      cullPass_.dispatchGroups(cmd, gs, {Binding::storageBuffer(drawList_), Binding::storageBuffer(entryBatch_),
                                         Binding::storageBuffer(cullBatches_), Binding::storageBuffer(commands),
                                         Binding::storageBuffer(culled), Binding::storageBuffer(visibleLast_),
                                         Binding::sampled(hiZ_.view, nearest_), Binding::storageBuffer(hlodInfo_),
                                         Binding::storageBuffer(hlodCells_)},
                               (entries + 63) / 64 + 1, 1, cp);
      barrier();
      if (meshlets) {  // grupos do task shader de cada lote a partir das instâncias visíveis
        const uint32_t ap[2] = {batchCount, ctx_->meshShaderProperties.maxTaskWorkGroupTotalCount};
        meshletArgsPass_.dispatchGroups(cmd, gs, {Binding::storageBuffer(commands), Binding::storageBuffer(meshletGroups_),
                                                  Binding::storageBuffer(meshletArgs_[phase])},
                                        (batchCount + 63) / 64 + 1, 1, ap);
        barrier();
      }
    };
    auto draw = [&](bool first, const rhi::Buffer& commands, const rhi::Buffer& culled, const rhi::Buffer& meshArgs) {
      const VkAttachmentLoadOp op = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      VkRenderingAttachmentInfo colors[6] = {
          attachment(t_.diffuse.view, VK_IMAGE_LAYOUT_GENERAL, op),
          attachment(t_.normal[cur].view, VK_IMAGE_LAYOUT_GENERAL, op),
          attachment(t_.motion.view, VK_IMAGE_LAYOUT_GENERAL, op),
          attachment(t_.emissive.view, VK_IMAGE_LAYOUT_GENERAL, op),
          attachment(t_.specular.view, VK_IMAGE_LAYOUT_GENERAL, op),
          attachment(t_.visibility[cur].view, VK_IMAGE_LAYOUT_GENERAL, op),  // 0 = sem objeto
      };
      VkRenderingAttachmentInfo depth = attachment(t_.depth[cur].view, VK_IMAGE_LAYOUT_GENERAL, op);
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
      VkPipeline bound = gbufferPipeline_;
      GBufferPush p{culled.address, commands.address};
      vkCmdPushConstants(cmd, gbufferLayout_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
      // Opacos pelo desenho indexado, opacos por meshlets e por fim os misturados (decals), por cima. Os de meshlets
      // depois de todos os indexados: uma troca de pipeline por passe, e o llvmpipe (Mesa 25.2) trava num desenho
      // indexado logo depois de um por mesh shader na mesma passada de renderização.
      for (uint32_t pass = 0; pass < 3; ++pass)
        for (uint32_t bi = 0; bi < drawBatches_.size(); ++bi) {
          const DrawBatch& b = drawBatches_[bi];
          if (b.blend != (pass == 2)) continue;
          if (pass < 2 && (pass == 1) != (meshlets && b.meshlets)) continue;
          if (!scene_->meshes[b.mesh].resident) continue;  // streaming: fora da memória (o culling já não pôs ninguém)
          const bool byMeshlets = meshlets && b.meshlets;
          const VkPipeline want = pass == 2 ? gbufferBlendPipeline_ : byMeshlets ? gbufferMeshPipeline_ : gbufferPipeline_;
          if (want != bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, want);
            bound = want;
          }
          const auto& m = scene_->meshes[b.mesh];
          vkCmdSetCullMode(cmd, b.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT);
          vkCmdSetFrontFace(cmd, b.mirrored != kFrontFaceClockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
          if (byMeshlets) {
            p.meshlets = m.meshlets.address;
            p.vertices = m.vertices.address;
            p.batch = bi;
            p.meshletCount = m.meshletCount;
            p.vertexBase = m.meshletVertexBase;
            p.triangleBase = m.meshletTriangleBase;
            p.flags = (settings.gpuCulling ? 1u : 0u) | (b.doubleSided ? 2u : 0u);
            vkCmdPushConstants(cmd, gbufferLayout_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(p), &p);
            ctx_->vkCmdDrawMeshTasksIndirectEXT(cmd, meshArgs.handle, VkDeviceSize(bi) * 3 * sizeof(uint32_t), 1,
                                                3 * sizeof(uint32_t));
            continue;
          }
          VkBuffer vbs[2] = {m.vertices.handle, m.deformable() ? m.prevVertices.handle : m.vertices.handle};
          VkDeviceSize offsets[2] = {0, 0};
          vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offsets);
          vkCmdBindIndexBuffer(cmd, m.indices.handle, 0, VK_INDEX_TYPE_UINT32);
          vkCmdDrawIndexedIndirect(cmd, commands.handle, VkDeviceSize(bi) * sizeof(VkDrawIndexedIndirectCommand), 1,
                                   sizeof(VkDrawIndexedIndirectCommand));
        }
      vkCmdEndRendering(cmd);
    };
    cull(0, drawCommands_, culledList_);
    profiler_.mark(cmd, "culling");
    draw(true, drawCommands_, culledList_, meshletArgs_[0]);
    barrier();
    if (occlusion) {
      // Pirâmide da profundidade da fase 0 (nível 0 = cópia; depois o mais distante de cada 2x2).
      const uint32_t fromDepth = 1, fromLevel = 0;
      for (uint32_t m = 0; m < hiZ_.mipLevels; ++m) {
        const VkExtent2D e{std::max(1u, hiZ_.extent.width >> m), std::max(1u, hiZ_.extent.height >> m)};
        hiZBuildPass_.dispatch(cmd, gs, {m == 0 ? tex(t_.depth[cur]) : Binding::sampled(hiZMips_[m - 1], nearest_),
                                         Binding::storageImage(hiZMips_[m])}, e, m == 0 ? &fromDepth : &fromLevel);
        barrier();
      }
      cull(1, drawCommands2_, culledList2_);
      draw(false, drawCommands2_, culledList2_, meshletArgs_[1]);
    }
  }
  cpuMark("desenho");
  barrier();
  profiler_.mark(cmd, "gbuffer");

  const Binding gDepth = tex(t_.depth[cur]), gNormal = tex(t_.normal[cur]), gDiffuse = tex(t_.diffuse),
                gSpecular = tex(t_.specular);
  const Binding gDepthPrev = tex(t_.depth[prev]), gNormalPrev = tex(t_.normal[prev]), gMotion = tex(t_.motion);

  // ---- 2b. Shadow maps em cascata (sem ray tracing ou --shadow-maps) ----
  if (useShadowMaps_ && u.shadowSplits.w > 0.0f) {
    for (uint32_t c = 0; c < 4; ++c) {
      VkRenderingAttachmentInfo depth = attachment(shadowLayerViews_[c], VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
      depth.clearValue.depthStencil = {1.0f, 0};
      VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
      ri.renderArea = {{0, 0}, shadowMap_.extent};
      ri.layerCount = 1;
      ri.pDepthAttachment = &depth;
      vkCmdBeginRendering(cmd, &ri);
      setViewport(cmd, shadowMap_.extent);
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowLayout_.pipelineLayout, 0, 1, &gs, 0, nullptr);
      struct { VkDeviceAddress drawList; uint32_t cascade, pad; } sp{drawList_.address, c, 0};
      vkCmdPushConstants(cmd, shadowLayout_.pipelineLayout, VK_SHADER_STAGE_ALL, 0, sizeof(sp), &sp);
      for (const DrawBatch& b : drawBatches_) {
        if (b.lod != 0 || b.hlodProxy) continue;  // sombra com a malha completa (LOD e HLOD repetem os objetos)
        if (!scene_->meshes[b.mesh].resident) continue;  // streaming
        const auto& m = scene_->meshes[b.mesh];
        VkBuffer vbs[2] = {m.vertices.handle, m.vertices.handle};
        VkDeviceSize offsets[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offsets);
        vkCmdBindIndexBuffer(cmd, m.indices.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, m.indexCount, b.count, 0, 0, b.first);
      }
      vkCmdEndRendering(cmd);
    }
    barrier();
    profiler_.mark(cmd, "sombra_cascatas");
  }

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

  // ---- 5b. Espalhamento subsuperficial (pele): borra a irradiância difusa dos pixels com SSS ----
  if (hasSubsurface_ && settings.subsurface) {
    const glm::ivec2 horizontal(1, 0), vertical(0, 1);
    sssPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.visibility[cur]), tex(*diffuseFinal), storage(t_.sssA)}, R,
                      &horizontal);
    barrier();
    sssPass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.visibility[cur]), tex(t_.sssA), storage(t_.sssB)}, R,
                      &vertical);
    barrier();
    diffuseFinal = &t_.sssB;
  }

  if (!showValidation) {
  // ---- 6. Composição ----
  compositePass_.dispatch(cmd, gs, {gDepth, gNormal, gDiffuse, gSpecular, tex(t_.emissive), tex(*diffuseFinal), tex(*specularFinal),
                                    storage(t_.sceneColor), Binding::sampled(t_.clouds[cur].view, linear_),
                                    Binding::sampled(skyClear_.view, linear_), Binding::sampled(t_.fogIntegrated.view, linear_)}, R);
  barrier();
  if (particlePipeline_ != VK_NULL_HANDLE) {
    VkRenderingAttachmentInfo color = attachment(t_.sceneColor.view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_LOAD);
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, R};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    setViewport(cmd, R);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline_);
    const ParticlePush pp{uint32_t(scene_->emitters.size()), ctx_->rayTracing ? 1u : 0u, scene_->time, 0};
    particleLayout_.bind(*ctx_, cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gs,
                         {Binding::storageBuffer(emitterBuffer_), Binding::sampled(t_.depth[cur].view, nearest_)}, &pp);
    vkCmdDraw(cmd, 6, scene_->particleCount, 0, 0);
    vkCmdEndRendering(cmd);
    barrier();
  }
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
                 sharpness, settings.lensFlare};
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
  // Streaming: endereços das malhas que acabaram de entrar no buffer de instâncias (antes dos passes que o leem).
  for (uint32_t o : s.takeInstanceUpdates()) {
    struct { uint64_t vertices, indices; uint32_t material, pad0; uint64_t colors; } inst{0, 0, s.objects[o].material, 0, 0};
    s.instanceAddresses(s.objects[o].mesh, inst.vertices, inst.indices, inst.colors);
    vkCmdUpdateBuffer(cmd, s.instances().handle, VkDeviceSize(o) * sizeof(inst), sizeof(inst), &inst);
  }
  const auto& changed = s.changedObjects();
  for (uint32_t k = 0; k < kFramesInFlight; ++k)
    pendingObjects_[k].insert(pendingObjects_[k].end(), changed.begin(), changed.end());
  if (!historyValid_) objectsFull_.fill(true);  // reinício: anterior = atual em todos os buffers
  auto* objs = static_cast<ObjectGpu*>(objectBuffers_[slot].mapped);
  auto writeObject = [&](uint32_t i) {
    const auto& ob = s.objects[i];
    glm::uvec4 anim = ob.crowdClip != ~0u
                          ? glm::uvec4(crowdFrameBase_[ob.crowdClip], ob.animFrame, historyValid_ ? ob.prevAnimFrame : ob.animFrame, 0)
                          : glm::uvec4(~0u, 0, 0, 0);
    // Bit 0: streaming (malha fora da memória = objeto oculto no culling); acima: poses do clipe (passo entre níveis).
    anim.w = (s.meshes[ob.mesh].resident ? 0u : 1u) | (ob.crowdClip != ~0u ? s.crowdClips[ob.crowdClip].frames << 1 : 0u);
    objs[i] = {ob.transform, historyValid_ ? ob.prevTransform : ob.transform, anim};
    if (ctx.rayTracing && s.tlasSlotOf(i) >= 0) s.tlas.writeInstance(slot, uint32_t(s.tlasSlotOf(i)), s.tlasInstance(i));
  };
  // Layout do TLAS mudou (streaming): as posições das instâncias mudaram — reescreve todas neste slot.
  const bool layoutChanged = ctx.rayTracing && tlasLayoutSeen_[slot] != s.tlasLayout();
  if (layoutChanged) {
    for (uint32_t i = 0; i < s.objects.size(); ++i)
      if (s.tlasSlotOf(i) >= 0) s.tlas.writeInstance(slot, uint32_t(s.tlasSlotOf(i)), s.tlasInstance(i));
    tlasLayoutSeen_[slot] = s.tlasLayout();
  }
  const bool full = objectsFull_[slot] || layoutChanged;
  if (objectsFull_[slot]) {
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
    if (!m.deformable()) continue;
    anySkinned = true;
    if (!m.skinned) continue;
    const auto& src = s.models[size_t(m.skinModel)].jointMatrices[size_t(m.skin)];
    std::copy(src.begin(), src.end(), joints + m.jointOffset);
  }
  vmaFlushAllocation(ctx.allocator, jointBuffers_[slot].allocation, 0, VK_WHOLE_SIZE);

  if (anySkinned) {
    // Guarda a pose anterior antes de sobrescrever (para os vetores de movimento da deformação).
    if (historyValid_) {
      for (const auto& m : s.meshes) {
        if (!m.deformable()) continue;
        VkBufferCopy region{0, 0, m.vertices.size};
        vkCmdCopyBuffer(cmd, m.vertices.handle, m.prevVertices.handle, 1, &region);
      }
      rhi::memoryBarrier(cmd);
    }
    // Tecido: simulação no intervalo de tempo da cena (zero com a cena pausada).
    const float sceneDt = historyValid_ ? std::max(s.time - clothTime_, 0.0f) : 0.0f;
    clothTime_ = s.time;
    for (const auto& m : s.meshes) {
      if (!m.cloth) continue;
      struct {
        VkDeviceAddress pos, prev, vertices;
        uint32_t width, height;
        float spacing, dt, time;
        uint32_t pinned;
        glm::vec4 colliders[4];
      } cp{m.clothPos.address, m.clothPrev.address, m.vertices.address, m.clothWidth, m.clothHeight, m.clothSpacing, sceneDt,
           s.time, (m.clothPinned ? 1u : 0u) | (std::getenv("LEGACY_NO_CLOTH_SELF") ? 2u : 0u), {m.clothColliders[0], m.clothColliders[1], m.clothColliders[2], m.clothColliders[3]}};
      clothPass_.dispatchGroups(cmd, globalSets_[slot], {}, 1, 1, &cp);
    }
    // Cabelo: física por fio no mundo (o objeto que usa a malha dá a posição da cabeça).
    for (uint32_t mi = 0; mi < s.meshes.size(); ++mi) {
      const auto& m = s.meshes[mi];
      if (!m.hair) continue;
      uint32_t object = ~0u;
      for (uint32_t i = 0; i < s.objects.size() && object == ~0u; ++i)
        if (s.objects[i].mesh == mi) object = i;
      if (object == ~0u) continue;
      struct {
        VkDeviceAddress rest, pos, prev, vertices;
        uint32_t object, strands, points;
        float dt, time;
        uint32_t reset;
      } hp{m.hairRest.address, m.hairPos.address, m.hairPrev.address, m.vertices.address, object, m.hairStrands,
           m.hairPoints, sceneDt, s.time, historyValid_ ? 0u : 1u};
      hairSimPass_.dispatchGroups(cmd, globalSets_[slot], {}, (m.hairStrands + 63) / 64, 1, &hp);
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
        if (!m.deformable()) continue;
        VkBufferCopy region{0, 0, m.vertices.size};
        vkCmdCopyBuffer(cmd, m.vertices.handle, m.prevVertices.handle, 1, &region);
      }
    }
    if (ctx.rayTracing) {
      for (auto& m : s.meshes) {
        if (!m.deformable()) continue;
        rhi::BlasInput in{m.vertices.address, m.vertexCount, sizeof(scene::Vertex), m.indices.address, m.indexCount};
        rhi::recordBlasUpdate(ctx, cmd, m.blas, in, rhi::alignScratch(ctx, s.skinScratch.address + m.updateScratchOffset));
      }
      rhi::memoryBarrier(cmd);
    }
  }
  cpuMark("esqueletos");
  // TLAS reconstruído só quando algo mudou (objeto movido ou malha com esqueleto refeita); parado, o da GPU vale.
  if (ctx.rayTracing && (objectsMoved || anySkinned || !tlasBuilt_)) {
    s.tlas.build(cmd, slot, s.tlasInstanceCount());
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

void Renderer::dumpCloth(const std::string& path) {
  rhi::Context& ctx = *ctx_;
  for (const auto& m : scene_->meshes) {
    if (!m.cloth) continue;
    const VkDeviceSize bytes = VkDeviceSize(m.clothWidth) * m.clothHeight * sizeof(glm::vec4);
    rhi::Buffer stage = rhi::createBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu);
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
      VkBufferCopy r{0, 0, bytes};
      vkCmdCopyBuffer(cmd, m.clothPos.handle, stage.handle, 1, &r);
    });
    vmaInvalidateAllocation(ctx.allocator, stage.allocation, 0, VK_WHOLE_SIZE);
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      const uint32_t header[2] = {m.clothWidth, m.clothHeight};
      const float spacing = m.clothSpacing;
      std::fwrite(header, sizeof(header), 1, f);
      std::fwrite(&spacing, sizeof(float), 1, f);
      std::fwrite(stage.mapped, 1, size_t(bytes), f);
      std::fclose(f);
    }
    rhi::destroyBuffer(ctx, stage);
    return;
  }
}

void Renderer::dumpHair(const std::string& path) {
  rhi::Context& ctx = *ctx_;
  scene::Scene& s = *scene_;
  for (uint32_t mi = 0; mi < s.meshes.size(); ++mi) {
    const auto& m = s.meshes[mi];
    if (!m.hair) continue;
    uint32_t object = 0;
    while (object < s.objects.size() && s.objects[object].mesh != mi) ++object;
    if (object == s.objects.size()) return;
    const VkDeviceSize bytes = VkDeviceSize(m.hairStrands) * m.hairPoints * sizeof(glm::vec4);
    rhi::Buffer stage = rhi::createBuffer(ctx, 2 * bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu);
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
      VkBufferCopy r0{0, 0, bytes}, r1{0, bytes, bytes};
      vkCmdCopyBuffer(cmd, m.hairRest.handle, stage.handle, 1, &r0);
      vkCmdCopyBuffer(cmd, m.hairPos.handle, stage.handle, 1, &r1);
    });
    vmaInvalidateAllocation(ctx.allocator, stage.allocation, 0, VK_WHOLE_SIZE);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
      const uint32_t header[2] = {m.hairStrands, m.hairPoints};
      std::fwrite(header, sizeof(header), 1, f);
      std::fwrite(&s.objects[object].transform[0][0], sizeof(glm::mat4), 1, f);
      std::fwrite(stage.mapped, 1, size_t(2 * bytes), f);
      std::fclose(f);
    }
    rhi::destroyBuffer(ctx, stage);
    return;
  }
}

void Renderer::destroy() {
  if (!ctx_) return;
  rhi::Context& ctx = *ctx_;
  vkDeviceWaitIdle(ctx.device);
  destroyTargets();
  profiler_.destroy();
  rhi::destroyImage(ctx, skyLut_);
  for (VkImageView v : shadowLayerViews_) vkDestroyImageView(ctx.device, v, nullptr);
  rhi::destroyImage(ctx, shadowMap_);
  vkDestroySampler(ctx.device, shadowSampler_, nullptr);
  if (shadowPipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device, shadowPipeline_, nullptr);
  shadowLayout_.destroy(ctx);
  for (rhi::Image* img : {&cloudNoise_, &skyClear_, &cloudShadow_, &rainOcclusion_}) rhi::destroyImage(ctx, *img);
  vkDestroySampler(ctx.device, cloudSampler_, nullptr);
  vkDestroySampler(ctx.device, oceanSampler_, nullptr);
  ocean_.destroy(ctx);
  ripplePass_.destroy(ctx);
  for (rhi::Image& img : ripple_) rhi::destroyImage(ctx, img);
  for (rhi::Buffer& b : rippleDisturbers_) rhi::destroyBuffer(ctx, b);
  for (rhi::Buffer& b : textureFeedback_) rhi::destroyBuffer(ctx, b);
  rhi::destroyBuffer(ctx, terrainLayers_);
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
  for (rhi::Buffer* b : {&entryBatch_, &cullBatches_, &drawCommands_, &culledList_, &crowdFrames_, &drawCommands2_,
                         &culledList2_, &visibleLast_, &meshletGroups_, &meshletArgs_[0], &meshletArgs_[1],
                         &hlodInfo_, &hlodCells_})
    rhi::destroyBuffer(ctx, *b);
  for (rhi::ComputePass* p : {&meshletArgsPass_, &hiZBuildPass_, &clothPass_, &hairSimPass_, &cullPass_, &cullResetPass_, &sssPass_, &rainOcclusionPass_, &cloudSkyPass_, &cloudShadowPass_, &cloudScreenPass_, &cloudReferencePass_, &fogInjectPass_, &fogIntegratePass_, &skinningPass_, &aerialPerspectivePass_, &radianceCacheResolvePass_, &skyLutPass_, &restirTemporalPass_, &restirSpatialPass_, &lightingPass_, &svgfTemporalPass_,
                              &svgfAtrousPass_, &compositePass_, &taaPass_, &rrGuidesPass_, &accumulatePass_, &histogramPass_, &exposurePass_, &dofPass_,
                              &motionBlurPass_, &bloomDownPass_, &bloomUpPass_})
    p->destroy(ctx);
  vkDestroyPipeline(ctx.device, gbufferPipeline_, nullptr);
  vkDestroyPipeline(ctx.device, gbufferBlendPipeline_, nullptr);
  if (gbufferMeshPipeline_) vkDestroyPipeline(ctx.device, gbufferMeshPipeline_, nullptr);
  if (particlePipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(ctx.device, particlePipeline_, nullptr);
  rhi::destroyBuffer(ctx, emitterBuffer_);
  particleLayout_.destroy(ctx);
  vkDestroyPipeline(ctx.device, tonemapPipeline_, nullptr);
  gbufferLayout_.destroy(ctx);
  tonemapLayout_.destroy(ctx);
  vkDestroyDescriptorPool(ctx.device, pool_, nullptr);
  vkDestroyDescriptorSetLayout(ctx.device, globalLayout_, nullptr);
  ctx_ = nullptr;
}

}  // namespace renderer
