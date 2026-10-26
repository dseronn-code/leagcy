#pragma once
#include <memory>

#include <glm/glm.hpp>

#include "rhi/context.h"

namespace renderer {

// Upscalers dos fabricantes pela extensão "Upscaler" do NVIDIA NRI (a mesma camada do NRD):
//   - DLSS Super Resolution (NVIDIA NGX, só GPUs RTX; licença NVIDIA DLSS — ver docs/licencas.md);
//   - AMD FSR 3 (FidelityFX SDK, MIT; o NRI só o compila no Windows).
//   - NVIDIA NIS (espacial, qualquer GPU): só para testar este caminho de integração em máquinas sem RTX —
//     não faz anti-aliasing temporal, então fica fora do uso normal.
// Substituem o passe de TAA/TAAU do motor. Se o tipo pedido não foi compilado ou a GPU não o suporta,
// init() falha e o renderer continua com o TAAU próprio.
class Upscaler {
 public:
  enum class Kind { None, Dlss, Fsr, Nis, Auto };  // Auto = DLSS se a GPU suportar, senão o TAAU do motor
  static const char* name(Kind k);

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    VkImageUsageFlags usage = 0;
  };
  struct Inputs {
    Image color;      // HDR linear pré-exposto, resolução de render
    Image depth[2];   // D32 com Z invertido e plano distante no infinito (alternadas a cada frame)
    Image motion;     // RG16F: (atual - anterior) em UV, sem jitter
    Image output[2];  // saídas alternadas (histórico do TAA), resolução final
  };
  struct Frame {
    glm::vec2 jitterPixels;  // deslocamento aplicado à imagem pela projeção, em pixels de render
    float zNear = 0.1f, verticalFov = 1.0f, frameTimeMs = 16.0f;
    uint32_t index = 0;  // qual das imagens alternadas (depth/output) é a do frame atual
    bool resetHistory = false;
  };

  Upscaler();
  ~Upscaler();
  // scale = resolução interna pedida (1,0 = DLAA/nativo; 0,67 = Qualidade; 0,5 = Desempenho...).
  bool init(rhi::Context& ctx, Kind kind, VkExtent2D output, float scale);
  void destroy();
  bool ready() const { return impl_ != nullptr; }
  Kind kind() const { return kind_; }
  VkExtent2D renderExtent() const { return renderExtent_; }
  uint32_t jitterPhases() const { return jitterPhases_; }
  float mipBias() const { return mipBias_; }

  // Embrulha as imagens (uma vez por recriação dos alvos). Todas ficam em VK_IMAGE_LAYOUT_GENERAL.
  bool bind(const Inputs& inputs);
  void dispatch(VkCommandBuffer cmd, const Frame& frame);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  rhi::Context* ctx_ = nullptr;
  Kind kind_ = Kind::None;
  VkExtent2D renderExtent_{};
  uint32_t jitterPhases_ = 16;
  float mipBias_ = 0.0f;
};

}  // namespace renderer
