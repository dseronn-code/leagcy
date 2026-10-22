#pragma once
#include <vector>

#include <glm/glm.hpp>

#include "rhi/accel.h"
#include "scene/mesh.h"

namespace scene {

// Material PBR metálico/rugosidade (mesmo modelo do glTF 2.0 / UE / Unity HDRP).
struct Material {
  glm::vec3 albedo{0.8f};
  float roughness = 0.5f;
  float metallic = 0.0f;
  glm::vec3 emissive{0.0f};
  uint32_t type = 0;  // 0 = padrão, 1 = terreno procedural (ver material.glsl)
};

struct Object {
  uint32_t mesh = 0;
  glm::mat4 transform{1.0f};
  Material material;
};

struct GpuMesh {
  rhi::Buffer vertices, indices;
  uint32_t indexCount = 0;
  rhi::AccelStructure blas;
};

class Scene {
 public:
  uint32_t addMesh(MeshData data) { cpuMeshes_.push_back(std::move(data)); return uint32_t(cpuMeshes_.size() - 1); }
  void addObject(const Object& o) { objects.push_back(o); }

  // Envia malhas para a GPU e constrói BLAS/TLAS (se houver ray tracing).
  void upload(rhi::Context& ctx);
  void destroy(rhi::Context& ctx);

  std::vector<Object> objects;
  std::vector<GpuMesh> meshes;
  rhi::AccelStructure tlas;

  glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.35f, -0.6f));  // direção PARA o sol
  float sunIntensity = 10.0f;  // irradiância (lux relativo)

 private:
  std::vector<MeshData> cpuMeshes_;
};

}  // namespace scene
