#pragma once
#include <array>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/resources.h"

namespace rhi {

struct AccelStructure {
  VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
  Buffer storage;
  VkDeviceAddress address = 0;
};

struct BlasInput {
  VkDeviceAddress vertexAddress = 0;
  uint32_t vertexCount = 0;
  uint32_t vertexStride = 0;  // posição (vec3 float) nos primeiros 12 bytes
  VkDeviceAddress indexAddress = 0;
  uint32_t indexCount = 0;    // uint32
};

struct TlasInstance {
  const AccelStructure* blas = nullptr;
  glm::mat4 transform{1.0f};
  uint32_t customIndex = 0;  // gl_InstanceCustomIndexEXT no shader
  bool opaque = true;        // false => o shader testa o alfa da textura (folhagem, grades)
};

// Bottom-level: triângulos de uma malha (otimizados para traçado rápido).
// allowUpdate: permite "refit" barato quando os vértices se movem (malhas com esqueleto).
AccelStructure buildBlas(Context& ctx, const BlasInput& in, bool allowUpdate = false);
// Scratch necessário para recordBlasUpdate (já inclui folga de alinhamento).
VkDeviceSize blasUpdateScratchSize(Context& ctx, const BlasInput& in);
// Refit no lugar: mesma topologia, posições novas. scratch deve estar alinhado.
void recordBlasUpdate(Context& ctx, VkCommandBuffer cmd, AccelStructure& blas, const BlasInput& in, VkDeviceAddress scratch);
VkDeviceAddress alignScratch(Context& ctx, VkDeviceAddress address);
void destroyAccel(Context& ctx, AccelStructure& as);

// Top-level reconstruído na GPU a cada frame: as instâncias (e suas transformações) mudam livremente.
// O handle do TLAS é estável, então os descritores não precisam ser reescritos.
class DynamicTlas {
 public:
  static constexpr uint32_t kSlots = 2;  // frames em voo

  void init(Context& ctx, uint32_t maxInstances);
  void record(VkCommandBuffer cmd, uint32_t slot, const std::vector<TlasInstance>& instances);
  void destroy();

  AccelStructure tlas;

 private:
  Context* ctx_ = nullptr;
  uint32_t maxInstances_ = 0;
  std::array<Buffer, kSlots> instanceBuffers_;
  Buffer scratch_;
};

}  // namespace rhi
