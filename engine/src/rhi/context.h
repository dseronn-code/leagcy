#pragma once
#include <functional>
#include <vector>

#include "rhi/vk_common.h"

namespace rhi {

// Funções de extensão de ray tracing (não exportadas estaticamente pelo loader).
struct RayTracingFns {
  PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
  PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
  PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
  PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
  PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
};

struct ContextDesc {
  bool validation = false;
  bool preferRayTracing = true;
  std::vector<const char*> instanceExtensions;  // ex.: as exigidas pelo GLFW
};

// Dono da instância, do dispositivo lógico, da fila principal e do alocador.
class Context {
 public:
  void createInstance(const ContextDesc& desc);
  // surface pode ser VK_NULL_HANDLE (modo headless).
  void createDevice(VkSurfaceKHR surface);
  void destroy();

  // Grava e executa um command buffer de uso único, esperando terminar — ou, entre beginDeferred() e endDeferred(),
  // grava no lote adiado (uma barreira de memória entre cada gravação) que endDeferred() envia SEM esperar.
  void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);
  // Envio adiado (streaming: a thread principal não espera a GPU copiar). O que precisa viver até a GPU terminar
  // (staging, scratch) vai em retire(): liberado quando o envio daquele lote termina (collectFinished, por frame).
  // Fora do modo adiado, retire() roda na hora.
  void beginDeferred();
  void endDeferred();
  void retire(std::function<void()> fn);
  void collectFinished(bool wait = false);

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;  // gráfica + compute + transferência (+ present)
  uint32_t queueFamily = 0;
  VmaAllocator allocator = VK_NULL_HANDLE;

  VkPhysicalDeviceProperties properties{};
  bool rayTracing = false;  // VK_KHR_ray_query + acceleration_structure ativos
  RayTracingFns rt;
  PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR = nullptr;
  // Mesh shaders (VK_EXT_mesh_shader, task + mesh): meshlets no G-buffer. Sem eles, desenho indexado comum.
  bool meshShaders = false;
  PFN_vkCmdDrawMeshTasksIndirectEXT vkCmdDrawMeshTasksIndirectEXT = nullptr;
  VkPhysicalDeviceMeshShaderPropertiesEXT meshShaderProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT};
  // Extensões habilitadas (bibliotecas que embrulham o nosso dispositivo, como o NRI, precisam saber).
  // Os nomes são literais estáticos dos headers do Vulkan/GLFW.
  std::vector<const char*> instanceExtensions, deviceExtensions;

 private:
  bool wantRayTracing_ = true;
  bool hasSurface_ = false;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkCommandPool uploadPool_ = VK_NULL_HANDLE;
  VkFence uploadFence_ = VK_NULL_HANDLE;
  VkCommandBuffer deferredCmd_ = VK_NULL_HANDLE;
  std::vector<std::function<void()>> deferredRetire_;
  struct DeferredBatch {
    VkFence fence;
    VkCommandBuffer cmd;
    std::vector<std::function<void()>> retire;
  };
  std::vector<DeferredBatch> inFlight_;
};

}  // namespace rhi
