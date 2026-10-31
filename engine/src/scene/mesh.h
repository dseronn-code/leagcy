#pragma once
#include <functional>
#include <vector>

#include <glm/glm.hpp>

namespace scene {

// 48 bytes; espelhado em shaders/scene.glsl (leitura via buffer device address no ray tracing).
struct Vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 uv;
  glm::vec4 tangent;  // xyz = tangente, w = sinal da bitangente
};
static_assert(sizeof(Vertex) == 48);

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  // Cor por vértice (glTF COLOR_0, linear; multiplica a cor base e o alfa). Vazio = branco.
  std::vector<glm::vec4> colors;
  // Skinning (opcional): até 4 juntas por vértice, índices relativos à skin do modelo.
  std::vector<glm::uvec4> joints;
  std::vector<glm::vec4> weights;
  int skinModel = -1;  // índice em Scene::models
  int skin = -1;       // índice em Model::skins
  // Tecido simulado (shaders/cloth.comp): grade de clothWidth x clothHeight vértices com espaçamento clothSpacing; a
  // coluna 0 fica presa (mastro, varal).
  uint32_t clothWidth = 0, clothHeight = 0;
  float clothSpacing = 0.0f;
  bool clothPinned = true;               // coluna 0 presa (bandeira); falso = cai livre (toalha)
  // Cabelo simulado (shaders/hair_sim.comp): fios consecutivos de hairPoints pontos (2 vértices de fita por ponto).
  uint32_t hairStrands = 0, hairPoints = 0;
  glm::vec4 clothColliders[4] = {};      // esferas de colisão no espaço do pano (xyz centro, w raio; w = 0 nenhuma)
};

// Versão simplificada (LOD) com ~`targetTriangles` triângulos: agrupamento de vértices numa grade com a posição de
// cada grupo pela minimização do erro quádrico dos triângulos que tocam o grupo (Lindstrom 2000, "Out-of-Core
// Simplification of Large Polygonal Models") — preserva quinas e silhuetas melhor que a média simples. Normais e UVs
// pela média do grupo, separados por lado da normal (não junta frente com verso de uma parede fina).
// groups != nullptr: posição pela média do grupo (sem erro quádrico) e devolve o grupo de cada vértice — o mesmo
// agrupamento serve para outras malhas da mesma topologia (poses assadas da multidão: applyVertexGroups).
MeshData simplifyMesh(const MeshData& mesh, uint32_t targetTriangles, std::vector<uint32_t>* groups = nullptr);
// Malha simplificada `lod` (de simplifyMesh com grupos) reconstruída para `pose` (mesma topologia da original): cada
// vértice do LOD é a média dos vértices do seu grupo nesta pose; os triângulos são os do LOD.
MeshData applyVertexGroups(const MeshData& pose, const std::vector<uint32_t>& groups, const MeshData& lod);

// Meshlets (pedaços de até 64 vértices e 124 triângulos) para os mesh shaders do G-buffer. Os triângulos de cada um são
// um trecho CONTÍNUO da lista de índices original, na mesma ordem: o gl_PrimitiveID (firstTriangle + i) é o mesmo do
// desenho indexado e o ray tracing/visibility buffer continuam valendo. Cada meshlet leva esfera envolvente e cone das
// normais (Wihlidal 2016; teste como no meshoptimizer) para o task shader descartar os fora da tela ou de costas.
struct Meshlet {  // == Meshlet em shaders/meshlet.glsl (48 bytes)
  glm::vec3 center;
  float radius;
  glm::vec3 coneAxis;
  float coneCutoff;        // seno do meio-ângulo do cone (1 = nunca descartar por estar de costas)
  uint32_t vertexOffset;   // em MeshletData::vertices
  uint32_t triangleOffset; // em MeshletData::triangles (um uint por triângulo: 3 índices locais de 8 bits)
  uint32_t counts;         // vértices | triângulos << 8
  uint32_t firstTriangle;  // primeiro triângulo na lista de índices original
};
static_assert(sizeof(Meshlet) == 48);
struct MeshletData {
  std::vector<Meshlet> meshlets;
  std::vector<uint32_t> vertices;   // índice do vértice da malha
  std::vector<uint32_t> triangles;  // índices locais empacotados
};
constexpr uint32_t kMeshletMaxVertices = 64, kMeshletMaxTriangles = 124;
MeshletData buildMeshlets(const MeshData& mesh);

// Cabelo por fios: `strands` fios com `segments` segmentos nascendo na calota de cima de uma esfera de raio
// `scalpRadius` (origem no centro), comprimento `length` (m), curvados pela gravidade. Cada segmento é uma fita: os
// vértices guardam a linha central ± halfWidth × normal (a fita parada que o ray tracing vê; o raster a vira para a
// câmera — gbuffer.vert), uv = (lado 0/1, posição no fio 0..1), tangente = direção do fio, tangent.w = meia largura.
MeshData makeHair(uint32_t strands, uint32_t segments, float scalpRadius, float length, float halfWidth, uint32_t seed);

// Pano em grade vertical (plano xy, pendurado para −y a partir da origem), coluna x = 0 presa.
MeshData makeClothGrid(uint32_t width, uint32_t height, float spacing);
// Pano horizontal (plano xz, centrado na origem), solto: cai e se acomoda sobre os colisores.
MeshData makeClothSheet(uint32_t width, uint32_t height, float spacing);

MeshData makeSphere(uint32_t segments = 96, uint32_t rings = 48);
MeshData makeCube();
// Disco no plano y = 0 com anéis em progressão geométrica (denso no centro, ralo na borda): a superfície do mar, que
// segue a câmera (o centro fica sob ela).
// Tufo de grama: `blades` lâminas finas e curvadas (3 segmentos cada) em volta da origem, altura ~`height` m.
MeshData makeGrassClump(uint32_t blades, float height, uint32_t seed);
MeshData makeRadialGrid(uint32_t rings, uint32_t segments, float innerRadius, float outerRadius);
// Grade de `res`x`res` quads cobrindo `size` metros; altura dada por heightFn(x, z).
MeshData makeTerrain(float size, uint32_t res, const std::function<float(float, float)>& heightFn);

float fbm(float x, float y, int octaves = 6);
// Tangentes por acúmulo das derivadas de UV de cada triângulo (aproxima MikkTSpace).
void computeTangents(MeshData& mesh);
// Normais suaves ponderadas por área (para malhas sem NORMAL, ex.: Fox do glTF-Sample-Assets).
void computeNormals(MeshData& mesh);

}  // namespace scene
