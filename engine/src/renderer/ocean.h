#pragma once
#include <vulkan/vulkan.h>

#include "rhi/pass.h"
#include "rhi/resources.h"

namespace renderer {

// Oceano por FFT (Tessendorf, "Simulating Ocean Water", 2001 — o método dos jogos de mundo aberto): espectro de
// Phillips com vento, evolução no tempo pela dispersão de águas profundas (ω² = g k) e IFFT 256² na GPU (uma passada
// por linha e uma por coluna, em memória compartilhada). Saídas, que repetem a cada kPatchSize metros:
//   displacement: xyz = deslocamento (ondas "choppy": as cristas afinam), w = Jacobiano (< 1 = crista dobrando);
//   slope:        xy = inclinação (∂h/∂x, ∂h/∂z), z = espuma (com memória: some aos poucos).
// Lidas pelo vertex shader da água (gbuffer.vert) e pelo material de água (material.glsl), set 0, bindings 17/18.
class Ocean {
 public:
  static constexpr uint32_t kSize = 256;
  static constexpr float kPatchSize = 256.0f;  // m

  // windSpeed em m/s; significantHeight = altura significativa das ondas (m), a média do terço mais alto.
  void init(rhi::Context& ctx, VkDescriptorSetLayout globalSet, float windSpeed, float windDirRad, float significantHeight);
  // Avança a simulação até `time` segundos.
  void update(VkCommandBuffer cmd, VkDescriptorSet globalSet, float time) const;
  void destroy(rhi::Context& ctx);

  const rhi::Image& displacement() const { return displacement_; }
  const rhi::Image& slope() const { return slope_; }
  bool ready() const { return ready_; }

 private:
  bool ready_ = false;
  rhi::Buffer h0Buffer_;
  rhi::Image spectrumA_, spectrumB_, displacement_, slope_;
  rhi::ComputePass spectrumPass_, fftPass_, finalizePass_;
};

}  // namespace renderer
