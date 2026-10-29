#pragma once
#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "rhi/accel.h"
#include "scene/mesh.h"
#include "scene/texture_compress.h"

namespace scene {

enum MaterialFlags : uint32_t {
  kMaterialAlphaMask = 1u << 0,  // recorte por alfa (folhas, grades)
  kMaterialTerrain = 1u << 1,    // terreno procedural (material.glsl)
  kMaterialSpecularGlossiness = 1u << 2,  // KHR_materials_pbrSpecularGlossiness
  // Emissão já amostrada como luz (luz de malha ou a esfera visível de uma luz analítica): nos pontos atingidos por
  // raios ela não soma, porque a amostragem de luzes (ReSTIR e a luz direta nos pontos atingidos) já a conta —
  // contar pelos dois caminhos dobraria a energia (auditoria 1.4).
  kMaterialEmissionSampled = 1u << 3,
  // Geometria visível de uma luz analítica (globo da lâmpada): transparente para os raios de sombra, senão a lâmpada
  // bloqueia a própria luz (auditoria 1.3).
  kMaterialLightGeometry = 1u << 4,
  kMaterialDoubleSided = 1u << 5,  // glTF doubleSided (luz de malha emite dos dois lados)
  // KHR_materials_transmission: vidro. Por enquanto opaco na imagem, mas não bloqueia os raios de sombra (a luz das
  // lanternas e janelas passa) — refração de verdade é o item 2.4 da auditoria.
  kMaterialTransmissive = 1u << 6,
  kMaterialClearcoat = 1u << 7,  // KHR_materials_clearcoat: verniz por cima (pintura de carro)
};

// Material PBR no modelo do glTF 2.0: metálico/rugosidade + KHR_materials_specular + KHR_materials_ior,
// ou especular/glossiness (kMaterialSpecularGlossiness). Espelhado em shaders/scene.glsl.
struct Material {
  glm::vec4 baseColor{1.0f};   // especular/glossiness: cor difusa
  glm::vec3 emissive{0.0f};    // luminância emitida em nits (cd/m²)
  float alphaCutoff = 0.5f;
  float metallic = 0.0f;
  float roughness = 0.5f;      // especular/glossiness: 1 - glossiness
  float normalScale = 1.0f;
  uint32_t flags = 0;
  int32_t baseColorTex = -1;
  int32_t metalRoughTex = -1;  // glTF: G = rugosidade, B = metalicidade | especular/glossiness: RGB + A
  int32_t normalTex = -1;
  int32_t emissiveTex = -1;
  glm::vec3 specularColor{1.0f};  // KHR_materials_specular | especular/glossiness: cor especular
  float specular = 1.0f;          // KHR_materials_specular: peso
  float ior = 1.5f;               // KHR_materials_ior (1,5 => f0 = 4%)
  int32_t specularTex = -1;
  int32_t specularColorTex = -1;
  int32_t occlusionTex = -1;  // glTF occlusionTexture (R): oclusão fina, só na luz indireta (o ray tracing faz o resto)
  // KHR_materials_transmission (parede fina, sem volume): fração da luz não refletida que atravessa, tingida pela cor
  // base (vidro de janela, garrafa). O difuso cai na mesma proporção.
  float transmission = 0.0f;
  int32_t transmissionTex = -1;  // R
  // KHR_materials_clearcoat: camada de verniz (dielétrico, f0 = 4%) sobre o material, com rugosidade própria e a
  // normal suave da geometria (o verniz cobre o relevo do normal map).
  float clearcoat = 0.0f;
  float clearcoatRoughness = 0.0f;
  int32_t clearcoatTex = -1;           // R
  int32_t clearcoatRoughnessTex = -1;  // G
  // Flocos metálicos (pintura de carro): fração das células com um floco inclinado e células por unidade de UV. De
  // perto cada floco brilha sozinho; de longe viram rugosidade a mais (a média dos flocos num pixel).
  float flakes = 0.0f;
  float flakeScale = 2000.0f;
};
static_assert(sizeof(Material) == 128);

enum LightType : uint32_t {
  kLightSphere = 0,    // esfera (pontual com tamanho) ou spot
  kLightTriangle = 1,  // triângulo emissivo de uma malha (luz de malha, como no RTXDI)
};

// Luz em unidades fotométricas. Esfera: intensidade luminosa em candela (cd) por canal; iluminância a uma distância
// d = intensity / d² (lux). Triângulo: vértice `position` + arestas `direction` e `edge2` (frente = cross das
// arestas), radiância vinda do material do objeto (com a textura emissiva no ponto); `intensity` guarda a radiância
// média (nits), usada só na escolha de luzes (BVH) e no alcance.
struct PointLight {
  glm::vec3 position{0.0f};
  float radius = 0.05f;  // tamanho físico em metros (penumbra)
  glm::vec3 intensity{100.0f};  // cd (lâmpada incandescente de 100 W ≈ 130 cd)
  float range = 10.0f;   // corte suave da influência (m)
  // Spot (KHR_lights_punctual): atenuação angular clamp(cos θ × spotScale + spotOffset, 0, 1)², com θ medido a partir
  // de `direction`; pontual = escala 0 e deslocamento 1 (sem atenuação).
  glm::vec3 direction{0.0f, -1.0f, 0.0f};  // triângulo: aresta 1
  float spotScale = 0.0f;
  glm::vec3 edge2{0.0f};  // triângulo: aresta 2
  float spotOffset = 1.0f;
  uint32_t type = kLightSphere;
  uint32_t object = 0, primitive = 0;  // triângulo: objeto e triângulo (para a textura emissiva)
  float area = 0.0f;                   // triângulo: área (m²)
};
static_assert(sizeof(PointLight) == 80);

// Centro e caixa de uma luz (BVH de luzes).
glm::vec3 lightCenter(const PointLight& l);
void lightBounds(const PointLight& l, glm::vec3& lo, glm::vec3& hi);
// Potência para a escolha de luzes: cd (esfera) ou radiância média × área (triângulo de frente).
float lightPower(const PointLight& l);

struct TextureData {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
  bool srgb = true;
  bool normalMap = false;  // normal map de espaço tangente: comprimido em BC5 (xy), z reconstruído no shader
  std::string name;  // arquivo de origem (diagnóstico)
  CompressedTexture precompressed;  // já em BC7 no disco (DDS): enviado como está; `rgba` fica vazio
  // Sampler do glTF: repetição em u e v (filtro: sempre trilinear + anisotrópico, o melhor disponível).
  VkSamplerAddressMode wrapU = VK_SAMPLER_ADDRESS_MODE_REPEAT, wrapV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  float alphaCutoff = -1.0f;  // >= 0: cor base de material com recorte por alfa (mips preservam a cobertura)
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

  // Texturas em BC7 (4x menos memória de vídeo; ver texture_compress.h). false = RGBA8 com mips na GPU.
  bool compressTextures = true;
  bool meshLights = true;  // triângulos emissivos viram luzes no upload
  std::string textureCacheDir = "texcache";  // cache dos BC7 (só a primeira carga comprime)

  uint32_t addMesh(MeshData data) { cpuMeshes_.push_back(std::move(data)); return uint32_t(cpuMeshes_.size() - 1); }
  uint32_t addMaterial(const Material& m) { materials.push_back(m); return uint32_t(materials.size() - 1); }
  uint32_t addTexture(TextureData t) { cpuTextures_.push_back(std::move(t)); return uint32_t(cpuTextures_.size() - 1); }
  // Marca a textura como cor base de material recortado por alfa (antes do upload).
  void setTextureAlphaCutoff(int32_t texture, float cutoff) {
    if (texture >= 0 && size_t(texture) < cpuTextures_.size()) cpuTextures_[size_t(texture)].alphaCutoff = cutoff;
  }
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
  rhi::TlasInstance tlasInstance(uint32_t object) const;
  // Move um objeto (animadores, animação glTF). Só os objetos movidos são reenviados à GPU e ao TLAS: numa cidade
  // parada o custo por frame não cresce com o número de objetos. Não altere `transform` diretamente após o upload.
  void setTransform(uint32_t object, const glm::mat4& transform);
  // Objetos cujos dados na GPU mudaram no último update(): os movidos agora e os movidos no frame anterior (o
  // prevTransform deles assentou). Pode ter repetições.
  const std::vector<uint32_t>& changedObjects() const { return changed_; }
  void destroy(rhi::Context& ctx);

  std::vector<Object> objects;
  std::vector<Material> materials;
  std::vector<PointLight> lights;
  std::vector<Model> models;
  std::vector<GpuMesh> meshes;
  std::vector<rhi::Image> textures;
  std::vector<std::pair<VkSamplerAddressMode, VkSamplerAddressMode>> textureWrap;  // por textura (u, v)
  rhi::DynamicTlas tlas;
  rhi::Buffer materialBuffer, instanceBuffer, skinScratch;
  uint32_t totalJoints = 0;
  float time = 0.0f;

  glm::vec3 sunDirection = glm::normalize(glm::vec3(0.4f, 0.35f, -0.6f));  // direção PARA o sol
  // Iluminância solar no topo da atmosfera (lux); a atmosfera atenua até ~100.000 lux ao meio-dia.
  float sunIlluminance = 120000.0f;  // 0 = noite
  bool sunFromAsset = false;  // direção do sol veio de uma luz direcional do glTF
  // Lua (à noite, quando sunIlluminance = 0): luz direcional com o disco no céu. Lua cheia ≈ 0,25 lux no chão.
  glm::vec3 moonDirection = glm::normalize(glm::vec3(-0.35f, 0.55f, 0.45f));
  float moonIlluminance = 0.0f;
  // Primeira câmera perspectiva do glTF (vista inicial definida pelo autor do asset).
  struct AssetCamera {
    bool valid = false;
    glm::vec3 position{0.0f}, forward{0.0f, 0.0f, -1.0f};
    float fovY = 1.0f, nearPlane = 0.1f;
  } assetCamera;
  // Névoa de altura (por metro ao nível do chão). Ainda sem sombra volumétrica (auditoria 3.7):
  // em interiores ela é iluminada pelo sol mesmo na sombra, então cenas fechadas usam valor baixo.
  float fogDensity = 0.0015f;
  float fogFalloff = 0.035f;  // queda exponencial com a altura
  // > 0: céu substituído por radiância uniforme (nits) — usado no teste da fornalha branca.
  float uniformSkyLuminance = 0.0f;

 private:
  void animateModel(Model& m, float dt);
  // Luzes de malha: cada triângulo de um objeto emissivo (sem esqueleto) vira uma luz amostrada pelo ReSTIR.
  struct MeshLightSource {
    uint32_t object, firstLight;
    std::vector<glm::vec3> local;  // 3 vértices por triângulo, espaço do objeto
    std::vector<uint32_t> primitives;
  };
  void buildMeshLights();
  void writeMeshLights(const MeshLightSource& src);
  std::vector<MeshLightSource> meshLightSources_;
  std::vector<int32_t> meshLightSourceOf_;  // objeto -> índice em meshLightSources_ (-1 = nenhum)

  std::vector<MeshData> cpuMeshes_;
  std::vector<uint32_t> changed_, movedNow_, movedLast_;
  std::vector<uint8_t> movedMark_;
  std::vector<TextureData> cpuTextures_;
  std::vector<Animator> animators_;
};

// Carrega um arquivo .gltf/.glb, adicionando malhas, materiais e texturas à cena.
// Se o arquivo tiver animações ou esqueletos, cria um Model e devolve seu índice em *modelOut (senão -1).
bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform, int* modelOut = nullptr);

}  // namespace scene
