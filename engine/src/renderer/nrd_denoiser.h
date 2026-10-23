#pragma once
#include <memory>

#include <glm/glm.hpp>

#include "rhi/context.h"

namespace renderer {

// NVIDIA NRD — ReLAX (difuso + especular), o denoiser que a NVIDIA recomenda para iluminação direta do
// ReSTIR + GI por ray tracing (o mesmo do Cyberpunk 2077 com path tracing). Integração pela camada oficial
// NRDIntegration + NRI, embrulhando o nosso VkDevice: o NRD cria os próprios pipelines e texturas internas
// e grava os despachos no nosso command buffer.
//
// Licença: "NVIDIA RTX SDKs License" — uso comercial permitido; o crédito à NVIDIA (marca do NRD) deve
// aparecer na tela de créditos de quem distribuir o motor ou um jogo feito com ele.
class NrdDenoiser {
 public:
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };
  struct Inputs {
    Image motion;           // RG: deslocamento em UV desde o frame anterior (atual - anterior)
    Image normalRoughness;  // codificação R10G10B10A2 do NRD
    Image viewZ;            // R32: z em espaço de câmera
    Image diffuse, specular;        // radiância (difuso demodulado) + distância do raio em .w
    Image outDiffuse, outSpecular;  // saída filtrada
  };
  struct Frame {
    glm::mat4 view, prevView, proj, prevProj;  // projeções SEM jitter
    glm::vec2 jitterPixels, prevJitterPixels;  // [-0,5; 0,5]
    uint32_t frameIndex = 0;
    float frameTimeMs = 0.0f;
    bool resetHistory = false;
  };

  NrdDenoiser();
  ~NrdDenoiser();
  // Todas as imagens ficam em VK_IMAGE_LAYOUT_GENERAL e voltam a ele depois de denoise().
  bool init(rhi::Context& ctx, VkExtent2D size, uint32_t framesInFlight);
  void destroy();
  void denoise(VkCommandBuffer cmd, const Inputs& in, const Frame& frame);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  VkExtent2D size_{};
};

}  // namespace renderer
