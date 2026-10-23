#pragma once
#include <array>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/pass.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace renderer {

struct Settings {
  float renderScale = 0.67f;        // resolução interna / saída (0.67 ~ "Quality" do FSR/DLSS)
  float exposureCompensation = 0.0f;  // EV somado à auto-exposição
  float manualExposure = 0.0f;      // > 0 desliga a auto-exposição
  float sunAngularRadius = 0.012f;  // rad (o sol real tem ~0.0047; maior = penumbra mais suave)
  float fogDensity = 0.0015f;       // por metro, ao nível do chão
  float fogFalloff = 0.035f;        // queda exponencial com a altura
  bool denoiser = true;
  bool depthOfField = false;
  float dofFocusDistance = 0.0f;    // 0 = autofoco no centro da tela
  float dofAperture = 8.0f;         // CoC (px) de um objeto no infinito
  float dofMaxCoc = 14.0f;
  bool motionBlur = true;
  float shutter = 0.5f;             // fração do frame com obturador aberto (0.5 = 180°)
  float bloomStrength = 0.05f;
};

// Frame (deferred, todos os alvos em VK_IMAGE_LAYOUT_GENERAL):
//   Sky LUT → G-buffer → ReSTIR (temporal, espacial) → Iluminação RT (difuso / especular)
//   → Denoiser SVGF (temporal + à-trous) → Composição (céu, névoa) → TAAU (upscale)
//   → Auto-exposição → DOF → Motion blur → Bloom → Tonemap ACES
class Renderer {
 public:
  static constexpr uint32_t kFramesInFlight = 2;

  void init(rhi::Context& ctx, scene::Scene& scene, VkExtent2D outputExtent, VkFormat outputFormat,
            const Settings& initialSettings = {});
  void resize(VkExtent2D outputExtent);
  void destroy();

  // Grava todos os passes. A imagem de saída termina em COLOR_ATTACHMENT_OPTIMAL.
  void record(VkCommandBuffer cmd, uint32_t frameSlot, const scene::Camera& camera, VkImage output,
              VkImageView outputView, float dt);

  // Descarta todo o histórico temporal (corte de câmera, mudança de cena).
  void resetHistory() { historyValid_ = false; }
  VkExtent2D renderExtent() const { return renderExtent_; }

  Settings settings;

 private:
  struct Targets {
    std::array<rhi::Image, 2> depth, normal;  // ping-pong: atual / anterior
    rhi::Image albedo, motion, emissive;
    rhi::Image diffuseRaw, specularRaw, diffuseIntegrated, specularIntegrated;
    std::array<rhi::Image, 2> diffuseHistory, specularHistory, moments, historyLength;
    rhi::Image tmpA, tmpB, tmpC, sceneColor;
    std::array<rhi::Image, 2> taa;
    rhi::Image postA, postB, bloom;
    std::vector<VkImageView> bloomMips;
    std::array<rhi::Buffer, 2> reservoirs;  // [0] = após reuso temporal, [1] = final (persistente)
  };

  void createTargets();
  void destroyTargets();
  void writeGlobalDescriptors();
  // Transformações, skinning, refit das BLAS e reconstrução do TLAS para este frame.
  void updateDynamicScene(VkCommandBuffer cmd, uint32_t slot);

  rhi::Context* ctx_ = nullptr;
  scene::Scene* scene_ = nullptr;
  VkExtent2D outputExtent_{}, renderExtent_{};
  VkFormat outputFormat_ = VK_FORMAT_UNDEFINED;
  Targets t_;

  rhi::Image skyLut_;
  VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE, skySampler_ = VK_NULL_HANDLE, textureSampler_ = VK_NULL_HANDLE;
  std::array<rhi::Buffer, kFramesInFlight> frameUbo_, objectBuffers_, jointBuffers_, lightBuffers_;
  rhi::Buffer histogram_, exposureState_;

  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kFramesInFlight> globalSets_{};

  rhi::ComputePass skinningPass_;
  rhi::ComputePass skyLutPass_, restirTemporalPass_, restirSpatialPass_, lightingPass_;
  rhi::ComputePass svgfTemporalPass_, svgfAtrousPass_, compositePass_, taaPass_;
  rhi::ComputePass histogramPass_, exposurePass_, dofPass_, motionBlurPass_, bloomDownPass_, bloomUpPass_;
  rhi::PassLayout gbufferLayout_, tonemapLayout_;
  VkPipeline gbufferPipeline_ = VK_NULL_HANDLE, tonemapPipeline_ = VK_NULL_HANDLE;

  uint64_t frameIndex_ = 0;
  bool historyValid_ = false;
  glm::mat4 prevViewProj_{1.0f};
  glm::vec3 prevCameraPos_{0.0f};
  glm::vec2 prevJitter_{0.0f};
};

}  // namespace renderer
