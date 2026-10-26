#include "renderer/upscaler.h"

#include <algorithm>
#include <cmath>

#include <NRI.h>
#include <Extensions/NRIDeviceCreation.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIUpscaler.h>
#include <Extensions/NRIWrapperVK.h>

#include "core/log.h"
#include "rhi/resources.h"

namespace renderer {

namespace {
// Modo do fabricante mais próximo da escala pedida (fatores por eixo: 1,0 / 1,3 / 1,5 / 1,7 / 2,0 / 3,0).
nri::UpscalerMode modeFor(float scale) {
  if (scale >= 0.9f) return nri::UpscalerMode::NATIVE;  // DLAA / FSR nativo
  if (scale >= 0.71f) return nri::UpscalerMode::ULTRA_QUALITY;
  if (scale >= 0.62f) return nri::UpscalerMode::QUALITY;
  if (scale >= 0.54f) return nri::UpscalerMode::BALANCED;
  if (scale >= 0.42f) return nri::UpscalerMode::PERFORMANCE;
  return nri::UpscalerMode::ULTRA_PERFORMANCE;
}

struct Wrapped {
  nri::Texture* texture = nullptr;
  nri::Descriptor* view = nullptr;
};
}  // namespace

struct Upscaler::Impl {
  nri::Device* device = nullptr;
  nri::CoreInterface core{};
  nri::WrapperVKInterface wrapper{};
  nri::UpscalerInterface upscaler{};
  nri::Upscaler* instance = nullptr;
  rhi::Image exposure;  // 1x1 com 1,0: a cor de entrada já vem pré-exposta pelo motor
  Wrapped color, depth[2], motion, exposureTex, output[2];
  VkImage colorImage = VK_NULL_HANDLE, depthImage[2] = {}, motionImage = VK_NULL_HANDLE;

  void release(Wrapped& w) {
    if (w.view) core.DestroyDescriptor(w.view);
    if (w.texture) core.DestroyTexture(w.texture);
    w = {};
  }
  void releaseAll() {
    for (Wrapped* w : {&color, &depth[0], &depth[1], &motion, &exposureTex, &output[0], &output[1]}) release(*w);
  }
  bool wrap(const Image& img, bool storage, Wrapped& out) {
    nri::TextureVKDesc td{};
    td.vkImage = (VKNonDispatchableHandle)img.image;
    td.vkFormat = (VKEnum)img.format;
    td.vkImageType = (VKEnum)VK_IMAGE_TYPE_2D;
    td.vkImageUsageFlags = (VKFlags)img.usage;
    td.width = nri::Dim_t(img.extent.width);
    td.height = nri::Dim_t(img.extent.height);
    td.depth = 1;
    td.mipNum = 1;
    td.layerNum = 1;
    td.sampleNum = 1;
    if (wrapper.CreateTextureVK(*device, td, out.texture) != nri::Result::SUCCESS) return false;
    nri::TextureViewDesc vd{};
    vd.texture = out.texture;
    vd.type = storage ? nri::TextureView::STORAGE_TEXTURE : nri::TextureView::TEXTURE;
    vd.format = nri::nriConvertVKFormatToNRI(uint32_t(img.format));
    vd.mipNum = 1;
    vd.layerNum = 1;
    vd.sliceNum = 1;
    return core.CreateTextureView(vd, out.view) == nri::Result::SUCCESS;
  }
};

const char* Upscaler::name(Kind k) {
  switch (k) {
    case Kind::Dlss: return "DLSS";
    case Kind::Fsr: return "FSR";
    case Kind::Nis: return "NIS";
    case Kind::Auto: return "automático";
    default: return "TAAU";
  }
}

Upscaler::Upscaler() = default;
Upscaler::~Upscaler() { destroy(); }

// Auto (o padrão) tenta o DLSS e, sem RTX, volta ao TAAU só com uma nota; pedido explícito que falha vira aviso.
#define UPSCALER_FALLBACK(...)  \
  do {                          \
    if (quiet) {                \
      LOG_INFO(__VA_ARGS__);    \
    } else {                    \
      LOG_WARN(__VA_ARGS__);    \
    }                           \
  } while (0)

bool Upscaler::init(rhi::Context& ctx, Kind kind, VkExtent2D output, float scale) {
  destroy();
  if (kind == Kind::None) return false;
  const bool quiet = kind == Kind::Auto;
  if (quiet) kind = Kind::Dlss;
  ctx_ = &ctx;
  auto impl = std::make_unique<Impl>();

  nri::QueueFamilyVKDesc queueFamily{};
  queueFamily.queueNum = 1;
  queueFamily.queueType = nri::QueueType::GRAPHICS;
  queueFamily.familyIndex = ctx.queueFamily;
  nri::DeviceCreationVKDesc deviceDesc{};
  deviceDesc.vkExtensions.instanceExtensions = ctx.instanceExtensions.data();
  deviceDesc.vkExtensions.instanceExtensionNum = uint32_t(ctx.instanceExtensions.size());
  deviceDesc.vkExtensions.deviceExtensions = ctx.deviceExtensions.data();
  deviceDesc.vkExtensions.deviceExtensionNum = uint32_t(ctx.deviceExtensions.size());
  deviceDesc.vkInstance = (VKHandle)ctx.instance;
  deviceDesc.vkDevice = (VKHandle)ctx.device;
  deviceDesc.vkPhysicalDevice = (VKHandle)ctx.gpu;
  deviceDesc.queueFamilies = &queueFamily;
  deviceDesc.queueFamilyNum = 1;
  deviceDesc.minorVersion = 3;
  if (nri::nriCreateDeviceFromVKDevice(deviceDesc, impl->device) != nri::Result::SUCCESS) {
    UPSCALER_FALLBACK("%s: falha ao embrulhar o dispositivo no NRI; usando o TAAU do motor", name(kind));
    return false;
  }
  Impl& m = *impl;
  impl_ = std::move(impl);  // a partir daqui destroy() limpa tudo
  if (nri::nriGetInterface(*m.device, NRI_INTERFACE(nri::CoreInterface), &m.core) != nri::Result::SUCCESS ||
      nri::nriGetInterface(*m.device, NRI_INTERFACE(nri::WrapperVKInterface), &m.wrapper) != nri::Result::SUCCESS ||
      nri::nriGetInterface(*m.device, NRI_INTERFACE(nri::UpscalerInterface), &m.upscaler) != nri::Result::SUCCESS) {
    UPSCALER_FALLBACK("%s: NRI sem a extensão de upscaler; usando o TAAU do motor", name(kind));
    destroy();
    return false;
  }
  const nri::UpscalerType type = kind == Kind::Dlss  ? nri::UpscalerType::DLSR
                                : kind == Kind::Fsr ? nri::UpscalerType::FSR
                                                    : nri::UpscalerType::NIS;
  if (!m.upscaler.IsUpscalerSupported(*m.device, type)) {
    UPSCALER_FALLBACK("%s indisponível (%s); usando o TAAU do motor", name(kind),
             kind == Kind::Dlss  ? "exige GPU NVIDIA RTX e o build com LEGACY_DLSS"
             : kind == Kind::Fsr ? "exige o build com LEGACY_FSR, só no Windows"
                                 : "build sem NRI_ENABLE_NIS_SDK");
    destroy();
    return false;
  }

  nri::UpscalerDesc ud{};
  ud.upscaleResolution = {nri::Dim_t(output.width), nri::Dim_t(output.height)};
  ud.type = type;
  ud.mode = modeFor(scale);
  ud.outputFormat = nri::Format::RGBA16_SFLOAT;  // só o NIS usa (alvo do TAA do motor)
  ud.flags = nri::UpscalerBits::HDR | nri::UpscalerBits::USE_EXPOSURE | nri::UpscalerBits::DEPTH_INVERTED |
             nri::UpscalerBits::DEPTH_INFINITE;
  // Sem command buffer: o NRI grava a criação no próprio e espera a GPU (chamado só fora do frame).
  if (m.upscaler.CreateUpscaler(*m.device, ud, m.instance) != nri::Result::SUCCESS) {
    UPSCALER_FALLBACK("%s: falha ao criar o upscaler; usando o TAAU do motor", name(kind));
    destroy();
    return false;
  }
  nri::UpscalerProps props{};
  m.upscaler.GetUpscalerProps(*m.instance, props);
  kind_ = kind;
  renderExtent_ = {props.renderResolution.w, props.renderResolution.h};
  // Halton(2,3) com pelo menos as fases que o fabricante pede (8·fator², ex.: 18 no Qualidade, 32 no Desempenho).
  jitterPhases_ = std::max<uint32_t>(16, props.jitterPhaseNum);
  mipBias_ = props.mipBias;

  m.exposure = rhi::createImage2D(ctx, {1, 1}, VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    rhi::imageBarrier(cmd, m.exposure.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue one{};
    one.float32[0] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, m.exposure.handle, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    rhi::imageBarrier(cmd, m.exposure.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
  });
  static const char* const kModes[] = {"nativo (AA)", "ultra qualidade", "qualidade", "equilibrado", "desempenho",
                                       "ultra desempenho"};
  LOG_INFO("%s ativo: modo %s, %ux%u -> %ux%u, %u fases de jitter, mip bias %.2f", name(kind), kModes[int(ud.mode)],
           renderExtent_.width, renderExtent_.height, output.width, output.height, jitterPhases_, mipBias_);
  return true;
}

void Upscaler::destroy() {
  if (!impl_) return;
  Impl& m = *impl_;
  m.releaseAll();
  if (m.instance) m.upscaler.DestroyUpscaler(m.instance);
  if (m.exposure.handle) rhi::destroyImage(*ctx_, m.exposure);
  if (m.device) nri::nriDestroyDevice(m.device);
  impl_.reset();
  kind_ = Kind::None;
}

bool Upscaler::bind(const Inputs& in) {
  if (!impl_) return false;
  Impl& m = *impl_;
  m.releaseAll();
  bool ok = m.wrap(in.color, false, m.color) && m.wrap(in.depth[0], false, m.depth[0]) && m.wrap(in.depth[1], false, m.depth[1]) && m.wrap(in.motion, false, m.motion) &&
            m.wrap(in.output[0], true, m.output[0]) && m.wrap(in.output[1], true, m.output[1]) &&
            m.wrap({m.exposure.handle, VK_FORMAT_R32_SFLOAT, {1, 1}, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
                   false, m.exposureTex);
  m.colorImage = in.color.image;
  m.depthImage[0] = in.depth[0].image;
  m.depthImage[1] = in.depth[1].image;
  m.motionImage = in.motion.image;
  if (!ok) LOG_WARN("%s: falha ao embrulhar as imagens", name(kind_));
  return ok;
}

void Upscaler::dispatch(VkCommandBuffer cmd, const Frame& f) {
  if (!impl_ || !impl_->color.texture) return;
  Impl& m = *impl_;
  const uint32_t i = f.index & 1;

  // DLSS e FSR leem as entradas em estado de leitura (SHADER_READ_ONLY_OPTIMAL no Vulkan) e escrevem a saída como
  // storage (GENERAL). O motor mantém tudo em GENERAL: transição de ida e volta só das três entradas.
  auto transition = [&](VkImageLayout from, VkImageLayout to) {
    for (VkImage img : {m.colorImage, m.motionImage})
      rhi::imageBarrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, from, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    rhi::imageBarrier(cmd, m.depthImage[i], VK_IMAGE_ASPECT_DEPTH_BIT, from, to, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                      VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
  };
  transition(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  nri::CommandBufferVKDesc cd{};
  cd.vkCommandBuffer = (VKHandle)cmd;
  cd.queueType = nri::QueueType::GRAPHICS;
  nri::CommandBuffer* cb = nullptr;
  if (m.wrapper.CreateCommandBufferVK(*m.device, cd, cb) == nri::Result::SUCCESS) {
    const Wrapped& out = m.output[i];
    nri::DispatchUpscaleDesc d{};
    d.output = {out.texture, out.view};
    d.input = {m.color.texture, m.color.view};
    d.guides.upscaler.mv = {m.motion.texture, m.motion.view};
    d.guides.upscaler.depth = {m.depth[i].texture, m.depth[i].view};
    d.guides.upscaler.exposure = {m.exposureTex.texture, m.exposureTex.view};
    if (kind_ == Kind::Fsr) {
      d.settings.fsr.zNear = f.zNear;
      d.settings.fsr.zFar = 1.0e6f;  // ignorado com DEPTH_INFINITE
      d.settings.fsr.verticalFov = f.verticalFov;
      d.settings.fsr.frameTime = f.frameTimeMs;
      d.settings.fsr.viewSpaceToMetersFactor = 1.0f;  // o motor trabalha em metros
      d.settings.fsr.sharpness = 0.0f;                // a nitidez é a RCAS do tonemap (--sharpness)
    } else if (kind_ == Kind::Nis) {
      d.settings.nis.sharpness = 0.0f;
    }
    d.currentResolution = {nri::Dim_t(renderExtent_.width), nri::Dim_t(renderExtent_.height)};
    // A projeção desloca a imagem em +jitter pixels (x para a direita, y para baixo): é o "camera jitter" do
    // DLSS/FSR, no mesmo sistema de coordenadas dos vetores de movimento (ver docs/auditoria.md, item 6).
    d.cameraJitter = {f.jitterPixels.x, f.jitterPixels.y};
    // Nossos vetores são (atual - anterior) em UV; o DLSS/FSR quer (anterior - atual) em pixels de render.
    d.mvScale = {-float(renderExtent_.width), -float(renderExtent_.height)};
    d.preExposure = 0.0f;  // a pré-exposição já está embutida e o histórico do upscaler acompanha a cena
    d.flags = f.resetHistory ? nri::DispatchUpscaleBits::RESET_HISTORY : nri::DispatchUpscaleBits::NONE;
    m.upscaler.CmdDispatchUpscale(*cb, *m.instance, d);
    m.core.DestroyCommandBuffer(cb);
  }

  transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
}

}  // namespace renderer
