#include "renderer/ocean.h"

#include <cmath>
#include <complex>
#include <random>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>

namespace renderer {

namespace {
constexpr float kGravity = 9.81f;
constexpr VkDescriptorType SI = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SB = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;

struct SpectrumPush {
  float time, patchSize;
  uint32_t size, pad;
};
struct FftPush {
  uint32_t vertical, pad0, pad1, pad2;
};
}  // namespace

void Ocean::init(rhi::Context& ctx, VkDescriptorSetLayout globalSet, float windSpeed, float windDirRad,
                 float significantHeight) {
  // Espectro inicial h0(k) = (ξr + i ξi) √(P(k)/2), Phillips: P(k) = exp(-1/(k Lw)²) / k⁴ × |k̂·ŵ|² × exp(-(k ℓ)²)
  // (Lw = V²/g: maior onda que o vento sustenta; ℓ corta as ondas minúsculas). A amplitude é ajustada para a altura
  // significativa pedida (Hs = 4 σ, com σ² = Σ |h0|² pela identidade de Parseval).
  const uint32_t N = kSize;
  const float L = kPatchSize, Lw = windSpeed * windSpeed / kGravity, small = Lw * 0.001f;
  const glm::vec2 wind(std::cos(windDirRad), std::sin(windDirRad));
  std::mt19937 rng(1337);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  std::vector<std::complex<float>> h0(N * N);
  double variance = 0.0;
  for (uint32_t m = 0; m < N; ++m)
    for (uint32_t n = 0; n < N; ++n) {
      glm::vec2 k(glm::two_pi<float>() * (float(n) - float(N / 2)) / L, glm::two_pi<float>() * (float(m) - float(N / 2)) / L);
      float kl = glm::length(k);
      float P = 0.0f;
      if (kl > 1e-6f) {
        float kw = glm::dot(k / kl, wind);
        P = std::exp(-1.0f / (kl * Lw * kl * Lw)) / (kl * kl * kl * kl) * kw * kw * std::exp(-kl * kl * small * small);
        if (kw < 0.0f) P *= 0.07f;  // ondas contra o vento quase não existem
      }
      std::complex<float> h(gauss(rng), gauss(rng));
      h0[m * N + n] = h * std::sqrt(P * 0.5f);
      variance += double(std::norm(h0[m * N + n]));
    }
  // Var(h) = Σ (|h0(k)|² + |h0(-k)|²) ≈ 2 Σ |h0|².
  const float scale = variance > 0.0 ? float(significantHeight / 4.0 / std::sqrt(2.0 * variance)) : 0.0f;
  std::vector<glm::vec4> packed(N * N);  // (h0(k), conj(h0(-k)))
  for (uint32_t m = 0; m < N; ++m)
    for (uint32_t n = 0; n < N; ++n) {
      std::complex<float> a = h0[m * N + n] * scale, b = std::conj(h0[((N - m) % N) * N + (N - n) % N]) * scale;
      packed[m * N + n] = glm::vec4(a.real(), a.imag(), b.real(), b.imag());
    }
  h0Buffer_ = rhi::createBufferWithData(ctx, packed.data(), packed.size() * sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  const VkImageUsageFlags storage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  spectrumA_ = rhi::createImage2D(ctx, {N, N}, VK_FORMAT_R32G32B32A32_SFLOAT, storage);
  spectrumB_ = rhi::createImage2D(ctx, {N, N}, VK_FORMAT_R32G32B32A32_SFLOAT, storage);
  displacement_ = rhi::createImage2D(ctx, {N, N}, VK_FORMAT_R16G16B16A16_SFLOAT, storage);
  slope_ = rhi::createImage2D(ctx, {N, N}, VK_FORMAT_R16G16B16A16_SFLOAT, storage);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    for (rhi::Image* img : {&spectrumA_, &spectrumB_, &displacement_, &slope_}) {
      rhi::imageBarrier(cmd, img->handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                        VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
      VkClearColorValue zero{};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(cmd, img->handle, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
  });
  spectrumPass_.init(ctx, "ocean_spectrum.comp", globalSet, {SB, SI, SI}, sizeof(SpectrumPush));
  fftPass_.init(ctx, "ocean_fft.comp", globalSet, {SI, SI}, sizeof(FftPush));
  finalizePass_.init(ctx, "ocean_finalize.comp", globalSet, {SI, SI, SI, SI});
  ready_ = true;
}

void Ocean::update(VkCommandBuffer cmd, VkDescriptorSet globalSet, float time) const {
  using rhi::Binding;
  const SpectrumPush sp{time, kPatchSize, kSize, 0};
  spectrumPass_.dispatch(cmd, globalSet, {Binding::storageBuffer(h0Buffer_), Binding::storageImage(spectrumA_.view),
                                          Binding::storageImage(spectrumB_.view)}, {kSize, kSize}, &sp);
  rhi::memoryBarrier(cmd);
  for (uint32_t vertical = 0; vertical < 2; ++vertical) {  // IFFT das linhas e depois das colunas
    const FftPush fp{vertical, 0, 0, 0};
    fftPass_.dispatchGroups(cmd, globalSet, {Binding::storageImage(spectrumA_.view), Binding::storageImage(spectrumB_.view)},
                            kSize, 1, &fp);
    rhi::memoryBarrier(cmd);
  }
  finalizePass_.dispatch(cmd, globalSet, {Binding::storageImage(spectrumA_.view), Binding::storageImage(spectrumB_.view),
                                          Binding::storageImage(displacement_.view), Binding::storageImage(slope_.view)},
                         {kSize, kSize});
  rhi::memoryBarrier(cmd);
}

void Ocean::destroy(rhi::Context& ctx) {
  if (!ready_) return;
  for (rhi::ComputePass* p : {&spectrumPass_, &fftPass_, &finalizePass_}) p->destroy(ctx);
  for (rhi::Image* img : {&spectrumA_, &spectrumB_, &displacement_, &slope_}) rhi::destroyImage(ctx, *img);
  rhi::destroyBuffer(ctx, h0Buffer_);
  ready_ = false;
}

}  // namespace renderer
