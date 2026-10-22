#pragma once
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "rhi/accel.h"
#include "scene/mesh.h"

namespace scene {

enum MaterialFlags : uint32_t {
  kMaterialAlphaMask = 1u << 0,  // recorte por alfa (folhas, grades)
  kMaterialTerrain = 1u << 1,    // terreno procedural (material.glsl)
};

// Material PBR metálico/rugosidade no modelo do glTF 2.0. Espelhado em shaders/scene.glsl.
struct Material {
  glm::vec4 baseColor{1.0f};
  glm::vec3 emissive{0.0f};
  float alphaCutoff = 0.5f;
  float metallic = 0.0f;
  float roughness = 0.5f;
  float normalScale = 1.0f;
  uint32_t flags = 0;
  int32_t baseColorTex = -1;
  int32_t metalRoughTex = -1;  // glTF: G = rugosidade, B = metalicidade
  int32_t normalTex = -1;
  int32_t emissiveTex = -1;
};
static_assert(sizeof(Material) == 64);

// Luz pontual esférica. intensity em "candela relativa": radiância ~ intensity / d².
struct PointLight {
  glm::vec3 position{0.0f};
  float radius = 0.05f;  // tamanho físico (penumbra)
  glm::vec3 intensity{1.0f};
  float range = 10.0f;   // corte suave da influência
};
static_assert(sizeof(PointLight) == 32);

struct TextureData {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool srgb = true;
};

struct Object {
  uint32_t mesh = 0;
  glm::mat4 transform{1.0f};
  uint32_t material = 0;
};

struct GpuMesh {
  rhi::Buffer vertices, indices;
  uint32_t indexCount = 0;
  rhi::AccelStructure blas;
};

class Scene {
 public:
  uint32_t addMesh(MeshData data) { cpuMeshes_.push_back(std::move(data)); return uint32_t(cpuMeshes_.size() - 1); }
  uint32_t addMaterial(const Material& m) { materials.push_back(m); return uint32_t(materials.size() - 1); }
  uint32_t addTexture(TextureData t) { cpuTextures_.push_back(std::move(t)); return uint32_t(cpuTextures_.size() - 1); }
  void addObject(uint32_t mesh, const glm::mat4& transform, uint32_t material) { objects.push_back({mesh, transform, material}); }
  void addLight(const PointLight& l) { lights.push_back(l); }

  // Caixa envolvente em espaço de mundo (válida antes do upload).
  void bounds(glm::vec3& lo, glm::vec3& hi) const;

  // Envia tudo para a GPU e constrói BLAS/TLAS (se houver ray tracing).
  void upload(rhi::Context& ctx);
  void destroy(rhi::Context& ctx);

  std::vector<Object> objects;
  std::vector<Material> materials;
  std::vector<PointLight> lights;
  std::vector<GpuMesh> meshes;
  std::vector<rhi::Image> textures;
  rhi::AccelStructure tlas;
  rhi::Buffer materialBuffer, instanceBuffer, lightBuffer;

  glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.35f, -0.6f));  // direção PARA o sol
  float sunIntensity = 20.0f;  // irradiância (lux relativo); 0 = noite

 private:
  std::vector<MeshData> cpuMeshes_;
  std::vector<TextureData> cpuTextures_;
};

// Carrega um arquivo .gltf/.glb, adicionando malhas, materiais e texturas à cena.
bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform);

}  // namespace scene
