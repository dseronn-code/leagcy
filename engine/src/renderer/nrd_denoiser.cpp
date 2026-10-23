#include "renderer/nrd_denoiser.h"

#include <cstring>

#include <NRD.h>
#include <NRI.h>
#include <Extensions/NRIHelper.h>
#include <Extensions/NRIWrapperVK.h>
#include <NRDIntegration.hpp>

#include "core/log.h"

namespace renderer {

namespace {
constexpr nrd::Identifier kRelax = 0;

void copyMatrix(float dst[16], const glm::mat4& m) { std::memcpy(dst, &m[0][0], sizeof(float) * 16); }  // coluna principal, como o NRD
}  // namespace

struct NrdDenoiser::Impl {
  nrd::Integration integration;
};

NrdDenoiser::NrdDenoiser() = default;
NrdDenoiser::~NrdDenoiser() { destroy(); }

bool NrdDenoiser::init(rhi::Context& ctx, VkExtent2D size, uint32_t framesInFlight) {
  destroy();
  impl_ = std::make_unique<Impl>();
  size_ = size;

  const nrd::LibraryDesc& lib = *nrd::GetLibraryDesc();
  nri::QueueFamilyVKDesc queueFamily{};
  queueFamily.queueNum = 1;
  queueFamily.queueType = nri::QueueType::GRAPHICS;
  queueFamily.familyIndex = ctx.queueFamily;

  nri::DeviceCreationVKDesc deviceDesc{};
  deviceDesc.vkBindingOffsets = {lib.spirvBindingOffsets.samplerOffset, lib.spirvBindingOffsets.textureOffset,
                                 lib.spirvBindingOffsets.constantBufferOffset, lib.spirvBindingOffsets.storageTextureAndBufferOffset};
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

  nrd::IntegrationCreationDesc integrationDesc{};
  std::strncpy(integrationDesc.name, "Legacy NRD", sizeof(integrationDesc.name) - 1);
  integrationDesc.residencyPriority = 0.0f;
  integrationDesc.resourceWidth = uint16_t(size.width);
  integrationDesc.resourceHeight = uint16_t(size.height);
  integrationDesc.queuedFrameNum = uint8_t(framesInFlight);
  integrationDesc.autoWaitForIdle = true;

  const nrd::DenoiserDesc denoisers[] = {{kRelax, nrd::Denoiser::RELAX_DIFFUSE_SPECULAR}};
  nrd::InstanceCreationDesc instanceDesc{};
  instanceDesc.denoisers = denoisers;
  instanceDesc.denoisersNum = 1;

  if (impl_->integration.RecreateVK(integrationDesc, instanceDesc, deviceDesc) != nrd::Result::SUCCESS) {
    LOG_ERROR("NRD: falha ao criar a instância (ReLAX)");
    impl_.reset();
    return false;
  }
  LOG_INFO("NRD %u.%u.%u: ReLAX difuso + especular (%ux%u)", lib.versionMajor, lib.versionMinor, lib.versionBuild,
           size.width, size.height);
  return true;
}

void NrdDenoiser::destroy() {
  if (impl_) impl_->integration.Destroy();
  impl_.reset();
}

void NrdDenoiser::denoise(VkCommandBuffer cmd, const Inputs& in, const Frame& f) {
  if (!impl_) return;
  nrd::Integration& nrd = impl_->integration;
  nrd.NewFrame();

  nrd::CommonSettings common{};
  copyMatrix(common.viewToClipMatrix, f.proj);
  copyMatrix(common.viewToClipMatrixPrev, f.prevProj);
  copyMatrix(common.worldToViewMatrix, f.view);
  copyMatrix(common.worldToViewMatrixPrev, f.prevView);
  // Nossos vetores guardam (atual - anterior) em UV; o NRD quer uvAnterior = uv + mv · escala.
  common.motionVectorScale[0] = -1.0f;
  common.motionVectorScale[1] = -1.0f;
  common.motionVectorScale[2] = 0.0f;
  common.cameraJitter[0] = f.jitterPixels.x;
  common.cameraJitter[1] = f.jitterPixels.y;
  common.cameraJitterPrev[0] = f.prevJitterPixels.x;
  common.cameraJitterPrev[1] = f.prevJitterPixels.y;
  common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = uint16_t(size_.width);
  common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = uint16_t(size_.height);
  common.timeDeltaBetweenFrames = f.frameTimeMs;
  common.denoisingRange = 100000.0f;  // 100 km; o céu é gravado além disso
  common.frameIndex = f.frameIndex;
  common.accumulationMode = f.resetHistory ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
  nrd.SetCommonSettings(common);

  nrd::RelaxSettings relax{};
  relax.enableAntiFirefly = true;  // vagalumes são exatamente os "brilhinhos"
  relax.minHitDistanceWeight = 0.05f;  // recomendado para sinais limpos de ReSTIR
  nrd.SetDenoiserSettings(kRelax, &relax);

  nrd::ResourceSnapshot snapshot{};
  snapshot.restoreInitialState = true;  // tudo volta para GENERAL
  auto resource = [](const Image& img) {
    nrd::Resource r{};
    r.vk.image = (VKNonDispatchableHandle)img.image;
    r.vk.format = (VKEnum)img.format;
    r.state = {nri::AccessBits::SHADER_RESOURCE | nri::AccessBits::SHADER_RESOURCE_STORAGE, nri::Layout::GENERAL,
               nri::StageBits::ALL};
    return r;
  };
  snapshot.SetResource(nrd::ResourceType::IN_MV, resource(in.motion));
  snapshot.SetResource(nrd::ResourceType::IN_NORMAL_ROUGHNESS, resource(in.normalRoughness));
  snapshot.SetResource(nrd::ResourceType::IN_VIEWZ, resource(in.viewZ));
  snapshot.SetResource(nrd::ResourceType::IN_DIFF_RADIANCE_HITDIST, resource(in.diffuse));
  snapshot.SetResource(nrd::ResourceType::IN_SPEC_RADIANCE_HITDIST, resource(in.specular));
  snapshot.SetResource(nrd::ResourceType::OUT_DIFF_RADIANCE_HITDIST, resource(in.outDiffuse));
  snapshot.SetResource(nrd::ResourceType::OUT_SPEC_RADIANCE_HITDIST, resource(in.outSpecular));

  nri::CommandBufferVKDesc cmdDesc{};
  cmdDesc.vkCommandBuffer = (VKHandle)cmd;
  cmdDesc.queueType = nri::QueueType::GRAPHICS;
  nrd.DenoiseVK(&kRelax, 1, cmdDesc, snapshot);
}

}  // namespace renderer
