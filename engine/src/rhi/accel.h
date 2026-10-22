#pragma once
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
AccelStructure buildBlas(Context& ctx, const BlasInput& in);
// Top-level: instâncias de BLAS com transformações.
AccelStructure buildTlas(Context& ctx, const std::vector<TlasInstance>& instances);
void destroyAccel(Context& ctx, AccelStructure& as);

}  // namespace rhi
