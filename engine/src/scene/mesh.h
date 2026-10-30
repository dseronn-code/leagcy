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
  // Skinning (opcional): até 4 juntas por vértice, índices relativos à skin do modelo.
  std::vector<glm::uvec4> joints;
  std::vector<glm::vec4> weights;
  int skinModel = -1;  // índice em Scene::models
  int skin = -1;       // índice em Model::skins
  // Tecido simulado (shaders/cloth.comp): grade de clothWidth x clothHeight vértices com espaçamento clothSpacing; a
  // coluna 0 fica presa (mastro, varal).
  uint32_t clothWidth = 0, clothHeight = 0;
  float clothSpacing = 0.0f;
};

// Pano em grade vertical (plano xy, pendurado para −y a partir da origem), coluna x = 0 presa.
MeshData makeClothGrid(uint32_t width, uint32_t height, float spacing);

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
