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

  // Grava e executa um command buffer de uso único, esperando terminar.
  void immediateSubmit(const std::function<void(VkCommandBuffer)>& record);

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;  // gráfica + compute + transferência (+ present)
  uint32_t queueFamily = 0;
  VmaAllocator allocator = VK_NULL_HANDLE;

  VkPhysicalDeviceProperties properties{};
  bool rayTracing = false;  // VK_KHR_ray_query + acceleration_structure ativos
  RayTracingFns rt;

 private:
  bool wantRayTracing_ = true;
  bool hasSurface_ = false;
  VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
  VkCommandPool uploadPool_ = VK_NULL_HANDLE;
  VkFence uploadFence_ = VK_NULL_HANDLE;
};

}  // namespace rhi
