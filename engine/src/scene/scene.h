#pragma once
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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
  glm::mat4 prevTransform{1.0f};  // do frame anterior: vetores de movimento por objeto
};

// ---- Animação (glTF): hierarquia de nós, clipes e esqueletos ----

struct AnimationSampler {
  std::vector<float> times;
  std::vector<glm::vec4> values;  // vec3 (translação/escala) ou quaternion (x, y, z, w)
  bool step = false;              // STEP; senão LINEAR (CUBICSPLINE usa só os valores-chave)
};
struct AnimationChannel {
  uint32_t node = 0;
  enum Path : uint8_t { kTranslation, kRotation, kScale } path = kTranslation;
  uint32_t sampler = 0;
};
struct Animation {
  std::string name;
  std::vector<AnimationSampler> samplers;
  std::vector<AnimationChannel> channels;
  float duration = 0.0f;
};
struct ModelNode {
  int parent = -1;
  glm::vec3 translation{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  glm::vec3 scale{1.0f};
};
struct Skin {
  std::vector<uint32_t> joints;  // índices de nós
  std::vector<glm::mat4> inverseBind;
};

// Instância de um glTF animado. O jogo controla root/animation/speed; o motor faz o resto.
struct Model {
  glm::mat4 root{1.0f};
  int animation = 0;  // clipe ativo (-1 = pose de repouso)
  float speed = 1.0f;
  float time = 0.0f;

  std::vector<ModelNode> nodes;
  std::vector<Animation> animations;
  std::vector<Skin> skins;
  std::vector<std::pair<uint32_t, uint32_t>> rigidObjects;  // (nó, objeto) — partes sem esqueleto
  std::vector<uint32_t> skinnedObjects;                    // transformação = root
  std::vector<std::vector<glm::mat4>> jointMatrices;       // saída por skin: global(junta) * inverseBind

  int findAnimation(const std::string& name) const;
};

struct GpuMesh {
  rhi::Buffer vertices, indices;
  uint32_t vertexCount = 0, indexCount = 0;
  rhi::AccelStructure blas;
  // Malhas com esqueleto: `vertices` é reescrito por compute a cada frame a partir de `restVertices`.
  bool skinned = false;
  rhi::Buffer restVertices, skinData, prevVertices;
  int skinModel = -1, skin = -1;
  uint32_t jointOffset = 0, jointCount = 0;  // posição no buffer global de juntas
  VkDeviceSize updateScratchOffset = 0;
};

class Scene {
 public:
  using Animator = std::function<void(Scene&, float time, float dt)>;

  uint32_t addMesh(MeshData data) { cpuMeshes_.push_back(std::move(data)); return uint32_t(cpuMeshes_.size() - 1); }
  uint32_t addMaterial(const Material& m) { materials.push_back(m); return uint32_t(materials.size() - 1); }
  uint32_t addTexture(TextureData t) { cpuTextures_.push_back(std::move(t)); return uint32_t(cpuTextures_.size() - 1); }
  uint32_t addObject(uint32_t mesh, const glm::mat4& transform, uint32_t material) {
    objects.push_back({mesh, transform, material, transform});
    return uint32_t(objects.size() - 1);
  }
  // Luzes podem ser movidas a cada frame (ex.: faróis); a quantidade é fixa após o upload.
  uint32_t addLight(const PointLight& l) { lights.push_back(l); return uint32_t(lights.size() - 1); }
  // Lógica de movimento chamada a cada frame (no jogo, isso viria da simulação).
  void addAnimator(Animator a) { animators_.push_back(std::move(a)); }

  // Caixa envolvente em espaço de mundo (válida antes do upload).
  void bounds(glm::vec3& lo, glm::vec3& hi) const;
  // Caixa envolvente da pose de repouso de um modelo (válida antes do upload).
  void modelBounds(int model, glm::vec3& lo, glm::vec3& hi) const;

  // Envia tudo para a GPU e constrói BLAS/TLAS (se houver ray tracing).
  void upload(rhi::Context& ctx);
  // Avança o tempo: guarda as transformações anteriores, roda animadores e anima os modelos.
  void update(float dt);
  // Instâncias de ray tracing com as transformações atuais.
  std::vector<rhi::TlasInstance> tlasInstances() const;
  void destroy(rhi::Context& ctx);

  std::vector<Object> objects;
  std::vector<Material> materials;
  std::vector<PointLight> lights;
  std::vector<Model> models;
  std::vector<GpuMesh> meshes;
  std::vector<rhi::Image> textures;
  rhi::DynamicTlas tlas;
  rhi::Buffer materialBuffer, instanceBuffer, skinScratch;
  uint32_t totalJoints = 0;
  float time = 0.0f;

  glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.35f, -0.6f));  // direção PARA o sol
  float sunIntensity = 20.0f;  // irradiância (lux relativo); 0 = noite

 private:
  void animateModel(Model& m, float dt);

  std::vector<MeshData> cpuMeshes_;
  std::vector<TextureData> cpuTextures_;
  std::vector<Animator> animators_;
};

// Carrega um arquivo .gltf/.glb, adicionando malhas, materiais e texturas à cena.
// Se o arquivo tiver animações ou esqueletos, cria um Model e devolve seu índice em *modelOut (senão -1).
bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform, int* modelOut = nullptr);

}  // namespace scene
