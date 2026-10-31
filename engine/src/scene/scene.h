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
  // Superfície do mar: no G-buffer a malha segue a câmera e é deslocada pelas ondas da FFT; nos raios é o plano parado,
  // com a normal e a espuma lidas da simulação. Ver renderer/ocean.h.
  kMaterialWater = 1u << 8,
  // Folhagem: balança com o vento no vertex shader (marcado no glTF para materiais de recorte por alfa e dois lados).
  kMaterialFoliage = 1u << 9,
  kMaterialSubsurface = 1u << 10,  // espalhamento subsuperficial (shaders/sss.comp)
  // glTF alphaMode BLEND: misturado no G-buffer por cima do que já está lá (decals de sujeira, marcas), sem escrever
  // profundidade; nos raios vale o recorte por alfa.
  kMaterialBlend = 1u << 11,
  // Olho procedural numa esfera de UV latitude/longitude (makeSphere): frente em +z; íris com a cor base, pupila,
  // anel límbico, esclera; córnea por verniz (clearcoat).
  kMaterialEye = 1u << 12,
  // Cabelo (makeHair): fitas viradas para a câmera no raster, transparência estocástica abaixo de um pixel,
  // sombreamento anisotrópico (Kajiya-Kay com as duas lobas de Scheuermann) pela tangente do fio.
  kMaterialHair = 1u << 13,
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
  // Espalhamento subsuperficial (pele, cera, mármore): distância média que a luz anda dentro do material por canal
  // (m). Pele: vermelho ~ 3x mais longe que o verde e ~ 6x mais que o azul. Zero = sem SSS.
  glm::vec3 subsurfaceRadius{0.0f};
  // KHR_materials_volume: vidro com volume (garrafa, bola de vidro). > 0 = refrata na entrada e na saída pelo IOR e
  // absorve pela lei de Beer: transmitância attenuationColor a cada attenuationDistance metros dentro do material.
  float thickness = 0.0f;
  glm::vec3 attenuationColor{1.0f};
  float attenuationDistance = 0.0f;  // 0 = sem absorção
  // KHR_texture_transform (da textura de cor base, aplicado a todas as do material — o caso dos exportadores):
  // uv' = R(rotation) × (uv × scale) + offset.
  glm::vec2 uvOffset{0.0f};
  glm::vec2 uvScale{1.0f};
  float uvRotation = 0.0f;
  float pad4[3] = {};
};
static_assert(sizeof(Material) == 192);

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

// Emissor de partículas (shaders/particles.glsl): fogo, fumaça, faíscas ou chuva (em volta da câmera).
enum ParticleType : uint32_t {
  kParticleFire = 0, kParticleSmoke = 1, kParticleSparks = 2, kParticleRain = 3, kParticleSplash = 4, kParticleExplosion = 5,
};
struct ParticleEmitter {
  glm::vec3 position{0.0f};
  uint32_t type = kParticleFire;
  float radius = 0.3f;  // base do emissor (m)
  float scale = 1.0f;   // tamanho (fogo/fumaça/faíscas) ou intensidade (chuva)
  uint32_t first = 0, count = 0;
};
static_assert(sizeof(ParticleEmitter) == 32);

// Decal procedural projetado por uma caixa (shaders/material.glsl): muda cor, rugosidade e brilho de qualquer
// superfície dentro dela — no G-buffer e nos raios (aparece nos reflexos).
enum DecalType : uint32_t {
  kDecalCrosswalk = 0,  // faixas de pedestre (tinta branca)
  kDecalLaneLine = 1,   // linha tracejada de pista (tinta)
  kDecalDirt = 2,       // sujeira/encardido (escurece, fica mais áspero)
  kDecalOil = 3,        // mancha de óleo (escura e lisa)
  kDecalCracks = 4,     // rachaduras (linhas escuras finas)
  kDecalTexture = 5,    // imagem (pichação, cartaz, placa): textura RGBA projetada pelo eixo z da caixa, em qualquer
                        // superfície virada para ela (paredes incluídas)
};
struct Decal {
  glm::mat4 worldToDecal{1.0f};  // leva o mundo ao cubo [-1, 1]³ da caixa
  glm::vec4 color{1.0f};         // cor da tinta (a = opacidade)
  uint32_t type = kDecalDirt;
  float seed = 0.0f;
  int32_t texture = -1;  // kDecalTexture: índice da textura (Scene::addTexture)
  float roughness = 0.6f;  // da tinta/papel
};
static_assert(sizeof(Decal) == 96);

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
  // Multidão (animação assada): clipe em Scene::crowdClips e pose deste frame / do anterior. ~0 = objeto comum.
  uint32_t crowdClip = ~0u, animFrame = 0, prevAnimFrame = 0;
  float animPhase = 0.0f;  // s somados ao tempo (cada pessoa num ponto diferente do passo)
};

// Clipe de animação "assado" para multidões: K poses do ciclo como malhas estáticas consecutivas (firstMesh ..
// firstMesh + frames − 1). Cada pessoa da multidão lê a pose do seu instante no vertex shader (sem skinning por
// pessoa) e, no ray tracing, aponta para a BLAS dessa pose.
struct CrowdClip {
  uint32_t firstMesh = 0, frames = 0;
  float duration = 1.0f;  // s do ciclo
  uint32_t material = 0;
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
  glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};  // caixa local (culling na GPU)
  // Tecido simulado: posições atual e anterior da simulação (vec4 por vértice).
  bool cloth = false;
  uint32_t clothWidth = 0, clothHeight = 0;
  float clothSpacing = 0.0f;
  bool clothPinned = true;
  glm::vec4 clothColliders[4] = {};
  rhi::Buffer clothPos, clothPrev;
  bool deformable() const { return skinned || cloth; }  // vértices reescritos na GPU a cada frame
  // Meshlets (só com mesh shaders e malha rígida): um buffer com [Meshlet × meshletCount][vértices][triângulos];
  // os dois últimos começam em meshletVertexBase / meshletTriangleBase (em uints).
  rhi::Buffer meshlets;
  rhi::Buffer colors;  // cor por vértice (vec4 linear), só se o glTF tiver COLOR_0
  uint32_t meshletCount = 0, meshletVertexBase = 0, meshletTriangleBase = 0;
  // Níveis de detalhe (só a rasterização; o ray tracing usa a malha completa): malhas simplificadas e a distância
  // (m) a partir da qual cada uma é usada. -1 = sem esse nível.
  int lod[2] = {-1, -1};
  float lodDistance[2] = {0.0f, 0.0f};
  // Streaming: malha de uma célula do mundo, na GPU só quando a câmera está perto (Scene::stream).
  bool resident = true;
  int cell = -1;  // -1 = sempre residente
};

// Célula de streaming: as malhas dela sobem para a GPU (buffers e BLAS) quando a câmera chega a loadDistance da
// borda e saem quando passa de unloadDistance (histerese: não fica carregando e descarregando na fronteira).
struct StreamCell {
  glm::vec3 center{0.0f};
  float radius = 0.0f;
  std::vector<uint32_t> meshes;
  bool resident = false;
};

class Scene {
 public:
  using Animator = std::function<void(Scene&, float time, float dt)>;

  // Texturas em BC7 (4x menos memória de vídeo; ver texture_compress.h). false = RGBA8 com mips na GPU.
  bool compressTextures = true;
  bool meshLights = true;  // triângulos emissivos viram luzes no upload
  bool meshLods = true;    // gera LODs das malhas grandes no upload
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
  std::vector<ParticleEmitter> emitters;
  std::vector<CrowdClip> crowdClips;
  // Streaming do mundo: células e a malha de cada uma (antes do upload). stream() a cada frame.
  std::vector<StreamCell> streamCells;
  float streamLoadDistance = 350.0f, streamUnloadDistance = 450.0f;  // m da borda da célula
  float streamBudgetMB = 24.0f;                                      // por chamada de stream()
  uint32_t addStreamCell(glm::vec3 center, float radius) {
    streamCells.push_back({center, radius, {}, false});
    return uint32_t(streamCells.size() - 1);
  }
  void setMeshCell(uint32_t mesh, uint32_t cell) {
    if (meshCellPending_.size() <= mesh) meshCellPending_.resize(mesh + 1, -1);
    meshCellPending_[mesh] = int(cell);
    streamCells[cell].meshes.push_back(mesh);
  }
  // Carrega/descarrega células pela posição da câmera (fora da gravação de comandos). Devolve se algo mudou.
  bool stream(rhi::Context& ctx, glm::vec3 camera);
  // Objetos cujas entradas do buffer de instâncias (endereços da malha) mudaram: o renderer reescreve na GPU.
  std::vector<uint32_t> takeInstanceUpdates() { return std::move(instanceUpdates_); }
  rhi::Buffer& instances() { return instanceBuffer; }
  size_t residentStreamBytes() const { return residentBytes_; }
  // TLAS compacto: só objetos com malha na memória entram (posição no TLAS por objeto, -1 = fora). O layout muda
  // quando o streaming carrega/descarrega; o renderer então reescreve todas as instâncias.
  int32_t tlasSlotOf(uint32_t object) const { return object < tlasSlot_.size() ? tlasSlot_[object] : -1; }
  uint32_t tlasInstanceCount() const { return tlasCount_; }
  uint32_t tlasLayout() const { return tlasLayout_; }
  // Endereços de vértices/índices de uma malha para o buffer de instâncias: a malha vazia (válida) se ela está fora
  // da memória — nenhum shader lê endereço inválido mesmo que um driver ignore a máscara da instância.
  // `colors`: cores por vértice (COLOR_0), 0 = sem.
  void instanceAddresses(uint32_t mesh, uint64_t& vertices, uint64_t& indices, uint64_t& colors) const {
    const bool r = meshes[mesh].resident;
    vertices = r ? meshes[mesh].vertices.address : emptyVertices_.address;
    indices = r ? meshes[mesh].indices.address : emptyIndices_.address;
    colors = r ? meshes[mesh].colors.address : 0;
  }
  // Assa `frames` poses do clipe ativo da malha com esqueleto do modelo (pele feita na CPU) e devolve o índice do
  // clipe. Chame antes do upload.
  uint32_t bakeCrowdClip(int model, uint32_t frames);
  // Põe uma pessoa da multidão: objeto com a pose 0 do clipe; o instante da animação vem de `phase` (s).
  uint32_t addCrowdAgent(uint32_t clip, const glm::mat4& transform, float phase);
  std::vector<Decal> decals;
  rhi::Buffer decalBuffer;
  // Caixa do decal: centro, meias-medidas (x, y = espessura da projeção, z) e giro em torno do eixo vertical.
  void addDecal(DecalType type, glm::vec3 center, glm::vec3 halfExtent, float yawRad, glm::vec4 color = glm::vec4(1.0f));
  // Decal de imagem numa parede: centro na superfície, normal da parede (para fora), largura e altura (m).
  void addWallDecal(int32_t texture, glm::vec3 center, glm::vec3 wallNormal, float width, float height, float roughness = 0.6f);
  uint32_t particleCount = 0;
  // Acrescenta um emissor com `count` partículas. Fogo também cria uma luz que tremula (a chama ilumina a cena).
  void addEmitter(ParticleType type, glm::vec3 position, float radius, float scale, uint32_t count);
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
  // Nuvens volumétricas (cúmulos entre 1,5 e 4 km): fração do céu coberta. 0 = céu limpo (sem custo).
  float cloudCoverage = 0.0f;
  // Chuva (0..1): superfícies molhadas (escurecem e brilham), poças nas superfícies planas, respingos nas poças.
  float wetness = 0.0f, puddles = 0.0f, rain = 0.0f;
  // Oceano (simulação FFT): objetos com material kMaterialWater viram a superfície do mar, que segue a câmera.
  bool ocean = false;
  float seaLevel = 0.0f;  // altura da superfície do mar (m)
  float windSpeed = 9.0f;       // m/s
  float windDirection = 0.6f;   // rad
  float waveHeight = 1.4f;      // altura significativa (m)
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
  std::vector<uint32_t> crowdObjects_;
  // Streaming: dados de CPU das malhas de células (ficam na RAM), objetos por malha, destruições adiadas.
  std::vector<int> meshCellPending_;
  std::vector<MeshData> streamData_;
  std::vector<std::vector<uint32_t>> streamMeshlets_;  // meshlets das malhas de célula, prontos para subir
  std::vector<std::vector<uint32_t>> objectsOfMesh_;
  std::vector<uint32_t> instanceUpdates_;
  struct PendingDestroy {
    uint32_t mesh;
    uint32_t framesLeft;
  };
  std::vector<PendingDestroy> pendingDestroy_;
  size_t residentBytes_ = 0;
  rhi::Context* ctx_ = nullptr;
  rhi::Buffer emptyVertices_, emptyIndices_;  // malha vazia: endereços válidos para objetos fora da memória
  std::vector<int32_t> tlasSlot_;
  uint32_t tlasCount_ = 0, tlasLayout_ = 0;
  void rebuildTlasLayout();
  void makeResident(uint32_t mesh);
  void evict(uint32_t mesh);
  std::vector<uint32_t> changed_, movedNow_, movedLast_;
  std::vector<uint8_t> movedMark_;
  std::vector<TextureData> cpuTextures_;
  std::vector<Animator> animators_;
};

// Carrega um arquivo .gltf/.glb, adicionando malhas, materiais e texturas à cena.
// Se o arquivo tiver animações ou esqueletos, cria um Model e devolve seu índice em *modelOut (senão -1).
bool loadGltf(Scene& scene, const std::string& path, const glm::mat4& rootTransform, int* modelOut = nullptr);

}  // namespace scene
