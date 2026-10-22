#pragma once
#include <array>

#include <glm/glm.hpp>

#include "rhi/resources.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace renderer {

struct Settings {
  float exposure = 0.35f;
  float sunAngularRadius = 0.012f;  // rad (o sol real tem ~0.0047; maior = penumbra mais suave)
  float fogDensity = 0.0015f;       // por metro, ao nível do chão
  float fogFalloff = 0.035f;        // queda exponencial com a altura
};

// Pipeline de um frame:
//   1. Sky-view LUT (compute)       — atmosfera física
//   2. Forward HDR (raster)         — PBR + ray queries (sombras, GI, reflexões)
//   3. Céu (fullscreen)             — só nos pixels sem geometria (teste de profundidade)
//   4. Acumulação (compute)         — convergência progressiva com câmera parada
//   5. Tonemap ACES (fullscreen)    — HDR -> imagem de saída
class Renderer {
 public:
  static constexpr uint32_t kFramesInFlight = 2;
  static constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;

  void init(rhi::Context& ctx, scene::Scene& scene, VkExtent2D extent, VkFormat outputFormat);
  void resize(VkExtent2D extent);
  void destroy();

  // Grava todos os passes. A imagem de saída termina em COLOR_ATTACHMENT_OPTIMAL.
  void record(VkCommandBuffer cmd, uint32_t frameSlot, const scene::Camera& camera, VkImage output, VkImageView outputView);

  void resetAccumulation() { accumFrame_ = 0; }
  uint32_t accumulatedFrames() const { return accumFrame_; }

  Settings settings;

 private:
  void createTargets();
  void destroyTargets();
  void writePostDescriptors();

  rhi::Context* ctx_ = nullptr;
  scene::Scene* scene_ = nullptr;
  VkExtent2D extent_{};
  VkFormat outputFormat_ = VK_FORMAT_UNDEFINED;

  rhi::Image hdr_, depth_, accum_, skyLut_;
  VkSampler skySampler_ = VK_NULL_HANDLE;
  std::array<rhi::Buffer, kFramesInFlight> frameUbo_;
  rhi::Buffer instanceBuffer_;

  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout frameLayout_ = VK_NULL_HANDLE, postLayout_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kFramesInFlight> frameSets_{};
  VkDescriptorSet postSet_ = VK_NULL_HANDLE;

  VkPipelineLayout framePipelineLayout_ = VK_NULL_HANDLE, postPipelineLayout_ = VK_NULL_HANDLE;
  VkPipeline skyLutPipeline_ = VK_NULL_HANDLE, forwardPipeline_ = VK_NULL_HANDLE, skyPipeline_ = VK_NULL_HANDLE;
  VkPipeline accumulatePipeline_ = VK_NULL_HANDLE, tonemapPipeline_ = VK_NULL_HANDLE;

  uint32_t accumFrame_ = 0;
  uint32_t frameCounter_ = 0;
};

}  // namespace renderer
