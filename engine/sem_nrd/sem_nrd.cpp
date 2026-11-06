// Build sem as bibliotecas da NVIDIA (LEGACY_NRD=OFF, usado no Android): o denoiser NRD e os upscalers do NRI
// respondem "indisponível" e o renderer segue com o SVGF e o TAA/TAAU do próprio motor. O alocador de memória
// (VMA), que no build completo vem compilado dentro do NRI, é compilado aqui.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include <vk_mem_alloc.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "core/log.h"
#include "renderer/nrd_denoiser.h"
#include "renderer/upscaler.h"

namespace renderer {

struct NrdDenoiser::Impl {};
NrdDenoiser::NrdDenoiser() = default;
NrdDenoiser::~NrdDenoiser() = default;
bool NrdDenoiser::init(rhi::Context&, VkExtent2D size, uint32_t, bool reblur) {
  size_ = size;
  reblur_ = reblur;
  LOG_INFO("NRD indisponível neste build; usando o SVGF do motor");
  return false;
}
void NrdDenoiser::destroy() {}
void NrdDenoiser::denoise(VkCommandBuffer, const Inputs&, const Frame&) {}

struct Upscaler::Impl {};
const char* Upscaler::name(Kind k) {
  switch (k) {
    case Kind::Dlss: return "DLSS";
    case Kind::Fsr: return "FSR";
    case Kind::Nis: return "NIS";
    case Kind::Auto: return "automático";
    case Kind::DlssRR: return "DLSS Ray Reconstruction";
    default: return "TAAU";
  }
}
Upscaler::Upscaler() = default;
Upscaler::~Upscaler() = default;
bool Upscaler::init(rhi::Context&, Kind kind, VkExtent2D, float, bool) {
  if (kind != Kind::Auto && kind != Kind::None) LOG_WARN("Upscaler %s indisponível neste build; usando o TAAU do motor", name(kind));
  return false;
}
void Upscaler::destroy() {}
bool Upscaler::bind(const Inputs&) { return false; }
void Upscaler::dispatch(VkCommandBuffer, const Frame&) {}

}  // namespace renderer
