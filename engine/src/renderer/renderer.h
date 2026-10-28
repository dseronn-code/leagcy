#pragma once
#include <array>
#include <limits>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/pass.h"
#include "renderer/light_bvh.h"
#include "renderer/nrd_denoiser.h"
#include "renderer/upscaler.h"
#include "rhi/profiler.h"
#include "scene/camera.h"
#include "scene/scene.h"

namespace renderer {

// Orçamentos de qualidade. Nada aqui é limitado por hardware: os presets só escolhem quanto
// trabalho gastar por pixel. Ultra é o padrão; Cinema não economiza em nada (fotos e vídeos).
struct Quality {
  float renderScale = 1.0f;         // resolução interna / saída
  uint32_t samplesPerPixel = 2;     // amostras de sombra, GI e reflexo por pixel por frame
  uint32_t restirCandidates = 16;   // luzes candidatas por pixel (RIS)
  uint32_t restirNeighbors = 8;     // vizinhos no reuso espacial
  uint32_t denoiseDiffuseIterations = 5;   // passadas à-trous (raio 1, 2, 4, 8, 16...)
  uint32_t denoiseSpecularIterations = 4;
  float rayTextureLod = 0.0f;       // mip das texturas vistas em reflexos e GI (0 = máxima nitidez)
  float fireflyLimit = 32.0f;       // luminância pré-exposta máxima de uma amostra indireta
  uint32_t skyLutWidth = 512, skyLutHeight = 256;
  uint32_t bloomMips = 7;
  uint32_t dofSamples = 64;
  uint32_t motionBlurSamples = 16;
  float anisotropy = 16.0f;
  bool restirBiasCorrection = true;  // raios que tiram o viés de visibilidade do reuso do ReSTIR (1 + vizinhos)
};

enum class QualityPreset { Low, Medium, High, Ultra, Cinema };
Quality qualityPreset(QualityPreset preset);
bool parseQualityPreset(const char* name, QualityPreset& out);  // baixo, medio, alto, ultra, cinema

struct Settings {
  Quality quality;
  float exposureCompensation = 0.0f;  // EV somado à auto-exposição (+1 = imagem 2x mais clara)
  float manualEV100 = std::numeric_limits<float>::quiet_NaN();  // definido = exposição manual (EV100)
  float sunAngularRadius = 0.00465f;  // rad: tamanho real do disco solar (penumbras físicas)
  bool denoiser = true;
  // false = SVGF próprio (padrão: medido melhor em movimento com o nosso sinal, ver docs/auditoria.md);
  // true = NVIDIA NRD (melhor com a câmera parada).
  bool nrd = false;
  bool nrdReblur = false;           // NRD: ReBLUR em vez de ReLAX
  bool nrdValidation = false;       // mostra a camada de validação do NRD (histórico, desoclusão, vetores)
  bool depthOfField = false;
  float dofFocusDistance = 0.0f;    // 0 = autofoco no centro da tela
  float dofAperture = 8.0f;         // CoC (px) de um objeto no infinito
  float dofMaxCoc = 14.0f;
  bool motionBlur = true;
  float shutter = 0.5f;             // fração do frame com obturador aberto (0.5 = 180°)
  float bloomStrength = 0.05f;
  // Nitidez RCAS (AMD FidelityFX) na saída: 0 = desligada, 1 = máxima; < 0 = automática (0,5 com o TAA do motor e o
  // FSR; 0 com o DLSS, que já sai nítido — medido na RTX 3070 Ti: 1,10x a referência com RCAS 0,5).
  float sharpness = -1.0f;
  // Upscaler: Auto (padrão) = DLSS em GPUs RTX, senão o TAA/TAAU do motor (medido na RTX 3070 Ti: erro contra a
  // referência 0,0404 com DLSS contra 0,0505 do TAAU em 67%); None = sempre o do motor; Dlss / Fsr = pedido explícito.
  Upscaler::Kind upscaler = Upscaler::Kind::Auto;
  bool energyCompensation = true;   // compensação de múltiplo espalhamento do BRDF (Turquin 2019)
  // Visualização de depuração (valor cru da iluminação, sem denoiser/TAA/tonemap): ver DebugView.
  uint32_t debugView = 0;
  // Depuração do ReSTIR: bit 0 desliga o reuso temporal, bit 1 o espacial (medir cada etapa isolada).
  uint32_t restirDisableMask = 0;
};

// Modos de --debug-view. Os valores ficam em [0, 1] e saem direto na tela (linear).
enum DebugView : uint32_t {
  kDebugNone = 0,
  kDebugSunVisibility = 1,  // "sol": visibilidade do sol (1 = iluminado) — mostra acne e terminator
  kDebugSunShading = 2,     // "sol-luz": só o difuso do sol (N·L × visibilidade × sombreamento do relevo)
  kDebugGeometricNormal = 3,  // "normal": normal geométrica (0,5 + 0,5 n)
  kDebugSunCosine = 4,      // "nl": N·L do sol sem sombra (referência para medir sombra falsa)
  // Luz direta das luzes locais (radiância × exposição manual de --exposure), codificada como v/(1+v):
  // estimativa do ReSTIR e referência por força bruta (todas as luzes no alcance, com sombra).
  kDebugLocalLights = 5,          // "luzes"
  kDebugLocalLightsReference = 6, // "luzes-ref"
};
bool parseDebugView(const char* name, uint32_t& out);

// Frame (deferred, todos os alvos em VK_IMAGE_LAYOUT_GENERAL):
//   Sky LUT → G-buffer → ReSTIR (temporal, espacial) → Iluminação RT (difuso / especular)
//   → Denoiser SVGF (temporal + à-trous) → Composição (céu, névoa) → TAAU, DLSS ou FSR (upscale)
//   (com DLSS Ray Reconstruction: sem SVGF/NRD; a composição recebe a iluminação ruidosa e o DLSS-RR faz os dois)
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

  // Validação da atmosfera: compara o céu do motor (LUTs) com um path tracer de referência e imprime
  // a tabela de erros. Devolve o erro relativo médio de luminância (céu acima do horizonte).
  float runAtmosphereTest(uint32_t pathsPerThread);

  // Descarta todo o histórico temporal (corte de câmera, mudança de cena).
  void resetHistory() { historyValid_ = false; }
  VkExtent2D renderExtent() const { return renderExtent_; }
  // Tempo médio de GPU por passe desde a última chamada (vazio se ainda não há medições).
  std::string gpuTimings() { return profiler_.report(); }

  Settings settings;

 private:
  struct Targets {
    std::array<rhi::Image, 2> depth, normal;  // ping-pong: atual / anterior
    rhi::Image diffuse, specular, motion, emissive;
    std::array<rhi::Image, 2> visibility;
    rhi::Image viewZ, nrdNormalRoughness, diffuseDenoised, specularDenoised;  // entradas/saídas do NRD
    rhi::Image nrdValidation;  // camada de depuração do NRD (RGBA8)
    // Guias do DLSS Ray Reconstruction (rr_guides.comp).
    rhi::Image rrDiffuseAlbedo, rrSpecularAlbedo, rrHitDistance, rrNormalRoughness;
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

  rhi::Image skyLut_, dfgLut_, transmittanceLut_, multiScatterLut_, aerialPerspective_;
  VkExtent2D skyLutExtent_{};
  VkSampler nearest_ = VK_NULL_HANDLE, linear_ = VK_NULL_HANDLE, skySampler_ = VK_NULL_HANDLE, textureSampler_ = VK_NULL_HANDLE;
  std::array<rhi::Buffer, kFramesInFlight> frameUbo_, objectBuffers_, jointBuffers_, lightBuffers_, lightBvhBuffers_;
  rhi::Buffer histogram_, exposureState_;

  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout globalLayout_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, kFramesInFlight> globalSets_{};

  rhi::ComputePass skinningPass_, aerialPerspectivePass_;
  rhi::ComputePass skyLutPass_, restirTemporalPass_, restirSpatialPass_, lightingPass_;
  rhi::ComputePass svgfTemporalPass_, svgfAtrousPass_, compositePass_, taaPass_, rrGuidesPass_;
  rhi::ComputePass histogramPass_, exposurePass_, dofPass_, motionBlurPass_, bloomDownPass_, bloomUpPass_;
  rhi::PassLayout gbufferLayout_, tonemapLayout_;
  VkPipeline gbufferPipeline_ = VK_NULL_HANDLE, tonemapPipeline_ = VK_NULL_HANDLE;

  rhi::GpuProfiler profiler_;
  uint64_t frameIndex_ = 0;
  bool historyValid_ = false;
  glm::mat4 prevViewProj_{1.0f};
  glm::mat4 prevInvViewProj_{1.0f};
  glm::mat4 prevView_{1.0f}, prevProjNoJitter_{1.0f};
  glm::vec2 prevJitterPixels_{0.0f};
  NrdDenoiser nrd_;
  Upscaler upscaler_;
  bool upscalerReady_ = false;
  bool nrdReady_ = false;  // com o jitter daquele frame (reconstrói a superfície anterior)
  std::vector<LightBvhNode> lightBvhNodes_;
  glm::vec3 prevCameraPos_{0.0f};
  glm::vec2 prevJitter_{0.0f};
};

}  // namespace renderer
