// O libvulkan.so do NDK para Android 11 (API 30) só exporta o Vulkan 1.1; as funções do 1.2/1.3 que o motor chama
// direto (renderização dinâmica, sync2, endereço de buffer, estado dinâmico) existem nos aparelhos com Vulkan 1.3, mas
// têm de ser buscadas em tempo de execução. Aqui elas são definidas e encaminhadas para o ponteiro buscado pela
// instância (os trampolins da instância servem para qualquer dispositivo criado a partir dela).
#include <vulkan/vulkan.h>

#include "core/log.h"

namespace {
VkInstance gInstance = VK_NULL_HANDLE;
template <class F> F load(const char* name) {
  auto f = reinterpret_cast<F>(vkGetInstanceProcAddr(gInstance, name));
  if (!f) FATAL("Este aparelho não tem %s (o jogo precisa de Vulkan 1.3)", name);
  return f;
}
}  // namespace

void legacyVk13SetInstance(VkInstance instance) { gInstance = instance; }

#define SHIM(ret, name, params, args)                                  \
  extern "C" ret VKAPI_CALL name params {                              \
    static auto f = load<PFN_##name>(#name);                           \
    return f args;                                                     \
  }

SHIM(void, vkCmdBeginRendering, (VkCommandBuffer c, const VkRenderingInfo* i), (c, i))
SHIM(void, vkCmdEndRendering, (VkCommandBuffer c), (c))
SHIM(void, vkCmdPipelineBarrier2, (VkCommandBuffer c, const VkDependencyInfo* d), (c, d))
SHIM(void, vkCmdSetCullMode, (VkCommandBuffer c, VkCullModeFlags m), (c, m))
SHIM(void, vkCmdSetFrontFace, (VkCommandBuffer c, VkFrontFace f2), (c, f2))
SHIM(void, vkCmdWriteTimestamp2, (VkCommandBuffer c, VkPipelineStageFlags2 s, VkQueryPool q, uint32_t i), (c, s, q, i))
SHIM(VkDeviceAddress, vkGetBufferDeviceAddress, (VkDevice d, const VkBufferDeviceAddressInfo* i), (d, i))
SHIM(VkResult, vkQueueSubmit2, (VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f2), (q, n, s, f2))
