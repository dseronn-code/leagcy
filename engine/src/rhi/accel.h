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
  // Máscara de raios (shaders/raytracing.glsl): os raios de sombra usam RAY_MASK_SHADOW = 0x01; instâncias sem esse
  // bit (globo de lâmpada, vidro) não fazem sombra.
  uint8_t mask = 0xFF;
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
  // Grava a instância `index` no buffer do `slot`. Só as que mudaram precisam ser regravadas: numa cidade parada
  // nada é escrito (medido: preencher 98 mil instâncias por frame custava ~30 ms de CPU).
  void writeInstance(uint32_t slot, uint32_t index, const TlasInstance& instance);
  // Reconstrói o TLAS a partir do buffer do `slot` (as primeiras `count` instâncias).
  void build(VkCommandBuffer cmd, uint32_t slot, uint32_t count);
  uint32_t capacity() const { return maxInstances_; }
  void destroy();

  AccelStructure tlas;

 private:
  Context* ctx_ = nullptr;
  uint32_t maxInstances_ = 0;
  std::array<Buffer, kSlots> instanceBuffers_;
  Buffer scratch_;
};

}  // namespace rhi
