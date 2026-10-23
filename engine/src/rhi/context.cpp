#include "rhi/context.h"

#include <cstring>

namespace rhi {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) LOG_ERROR("[vulkan] %s", data->pMessage);
  else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) LOG_WARN("[vulkan] %s", data->pMessage);
  return VK_FALSE;
}

bool hasLayer(const char* name) {
  uint32_t n = 0;
  vkEnumerateInstanceLayerProperties(&n, nullptr);
  std::vector<VkLayerProperties> layers(n);
  vkEnumerateInstanceLayerProperties(&n, layers.data());
  for (auto& l : layers) if (std::strcmp(l.layerName, name) == 0) return true;
  return false;
}

bool hasDeviceExtension(VkPhysicalDevice gpu, const char* name) {
  uint32_t n = 0;
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data());
  for (auto& e : exts) if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}

bool supportsRayQuery(VkPhysicalDevice gpu) {
  return hasDeviceExtension(gpu, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) &&
         hasDeviceExtension(gpu, VK_KHR_RAY_QUERY_EXTENSION_NAME) &&
         hasDeviceExtension(gpu, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

}  // namespace

void Context::createInstance(const ContextDesc& desc) {
  wantRayTracing_ = desc.preferRayTracing;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "Legacy";
  app.pEngineName = "Legacy Engine";
  app.apiVersion = VK_API_VERSION_1_3;

  std::vector<const char*> exts = desc.instanceExtensions;
  std::vector<const char*> layers;
  bool validation = desc.validation && hasLayer("VK_LAYER_KHRONOS_validation");
  if (desc.validation && !validation) LOG_WARN("Camada de validação não encontrada; seguindo sem ela.");
  if (validation) {
    layers.push_back("VK_LAYER_KHRONOS_validation");
    exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
  }

  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = uint32_t(exts.size());
  ci.ppEnabledExtensionNames = exts.data();
  instanceExtensions = exts;
  ci.enabledLayerCount = uint32_t(layers.size());
  ci.ppEnabledLayerNames = layers.data();
  VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));

  if (validation) {
    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = debugCallback;
    auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (create) create(instance, &mi, nullptr, &messenger_);
  }
}

void Context::createDevice(VkSurfaceKHR surface) {
  hasSurface_ = surface != VK_NULL_HANDLE;
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  if (n == 0) FATAL("Nenhuma GPU com Vulkan encontrada.");
  std::vector<VkPhysicalDevice> gpus(n);
  vkEnumeratePhysicalDevices(instance, &n, gpus.data());

  // Pontuação: Vulkan 1.3 obrigatório; prefere dedicada e com ray tracing.
  int bestScore = -1;
  for (auto candidate : gpus) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(candidate, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) continue;
    if (hasSurface_ && !hasDeviceExtension(candidate, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
    if (!hasDeviceExtension(candidate, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
    int score = 1;
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
    else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 100;
    if (supportsRayQuery(candidate)) score += 500;
    if (score > bestScore) { bestScore = score; gpu = candidate; }
  }
  if (!gpu) FATAL("Nenhuma GPU com Vulkan 1.3 + VK_KHR_push_descriptor encontrada.");
  vkGetPhysicalDeviceProperties(gpu, &properties);
  rayTracing = wantRayTracing_ && supportsRayQuery(gpu);

  // Uma família que faça gráficos + compute (+ present, se houver janela).
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, qprops.data());
  bool found = false;
  for (uint32_t i = 0; i < qn && !found; ++i) {
    if ((qprops[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) !=
        (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) continue;
    VkBool32 present = VK_TRUE;
    if (hasSurface_) vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &present);
    if (present) { queueFamily = i; found = true; }
  }
  if (!found) FATAL("Nenhuma fila gráfica/compute/present adequada.");

  std::vector<const char*> exts = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  if (hasSurface_) exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  if (rayTracing) {
    exts.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    exts.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    exts.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
  }

  // Cadeia de features: Vulkan 1.2/1.3 core + ray tracing.
  VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
  rq.rayQuery = VK_TRUE;
  VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
  as.accelerationStructure = VK_TRUE;
  as.pNext = &rq;

  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  f13.maintenance4 = VK_TRUE;
  f13.pNext = rayTracing ? &as : nullptr;

  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.bufferDeviceAddress = VK_TRUE;
  f12.descriptorIndexing = VK_TRUE;
  f12.runtimeDescriptorArray = VK_TRUE;
  f12.descriptorBindingPartiallyBound = VK_TRUE;
  f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  f12.scalarBlockLayout = VK_TRUE;
  f12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
  f12.pNext = &f13;

  VkPhysicalDeviceFeatures supported{};
  vkGetPhysicalDeviceFeatures(gpu, &supported);
  // gl_PrimitiveID no fragment shader (visibility buffer). Toda GPU desktop tem.
  if (!supported.geometryShader) FATAL("A GPU não suporta geometryShader (gl_PrimitiveID no fragment shader).");
  // Formatos estendidos em storage (RG16F etc.): o NRD lê/escreve as entradas como imagens de storage.
  if (!supported.shaderStorageImageExtendedFormats) FATAL("A GPU não suporta shaderStorageImageExtendedFormats.");

  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.features.geometryShader = VK_TRUE;
  f2.features.shaderStorageImageExtendedFormats = VK_TRUE;
  f2.features.samplerAnisotropy = VK_TRUE;
  f2.features.shaderInt64 = VK_TRUE;
  f2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;
  f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
  f2.pNext = &f12;

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily;
  qci.queueCount = 1;
  qci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = uint32_t(exts.size());
  dci.ppEnabledExtensionNames = exts.data();
  deviceExtensions = exts;
  VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
  vkGetDeviceQueue(device, queueFamily, 0, &queue);
  vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR");

  if (rayTracing) {
#define LOAD_RT(fn) rt.fn = (PFN_##fn)vkGetDeviceProcAddr(device, #fn)
    LOAD_RT(vkCreateAccelerationStructureKHR);
    LOAD_RT(vkDestroyAccelerationStructureKHR);
    LOAD_RT(vkGetAccelerationStructureBuildSizesKHR);
    LOAD_RT(vkCmdBuildAccelerationStructuresKHR);
    LOAD_RT(vkGetAccelerationStructureDeviceAddressKHR);
#undef LOAD_RT
  }

  // A implementação do VMA vem compilada dentro do NRI (mesmo commit), no modo de funções dinâmicas:
  // o VMA busca os ponteiros do Vulkan a partir destes dois.
  VmaVulkanFunctions vmaFns{};
  vmaFns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  vmaFns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo aci{};
  aci.pVulkanFunctions = &vmaFns;
  aci.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  aci.physicalDevice = gpu;
  aci.device = device;
  aci.instance = instance;
  aci.vulkanApiVersion = VK_API_VERSION_1_3;
  VK_CHECK(vmaCreateAllocator(&aci, &allocator));

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queueFamily;
  VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &uploadPool_));
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VK_CHECK(vkCreateFence(device, &fci, nullptr, &uploadFence_));

  LOG_INFO("GPU: %s (Vulkan %u.%u.%u) | ray tracing: %s", properties.deviceName,
           VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
           VK_API_VERSION_PATCH(properties.apiVersion), rayTracing ? "SIM" : "não");
}

void Context::immediateSubmit(const std::function<void(VkCommandBuffer)>& record) {
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = uploadPool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VK_CHECK(vkAllocateCommandBuffers(device, &ai, &cmd));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  record(cmd);
  VK_CHECK(vkEndCommandBuffer(cmd));

  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
  cbi.commandBuffer = cmd;
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  VK_CHECK(vkQueueSubmit2(queue, 1, &si, uploadFence_));
  VK_CHECK(vkWaitForFences(device, 1, &uploadFence_, VK_TRUE, UINT64_MAX));
  VK_CHECK(vkResetFences(device, 1, &uploadFence_));
  vkFreeCommandBuffers(device, uploadPool_, 1, &cmd);
}

void Context::destroy() {
  if (!device) return;
  vkDeviceWaitIdle(device);
  vkDestroyFence(device, uploadFence_, nullptr);
  vkDestroyCommandPool(device, uploadPool_, nullptr);
  vmaDestroyAllocator(allocator);
  vkDestroyDevice(device, nullptr);
  if (messenger_) {
    auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroyFn) destroyFn(instance, messenger_, nullptr);
  }
  vkDestroyInstance(instance, nullptr);
  device = VK_NULL_HANDLE;
}

}  // namespace rhi
