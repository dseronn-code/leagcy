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
};

MeshData makeSphere(uint32_t segments = 96, uint32_t rings = 48);
MeshData makeCube();
// Grade de `res`x`res` quads cobrindo `size` metros; altura dada por heightFn(x, z).
MeshData makeTerrain(float size, uint32_t res, const std::function<float(float, float)>& heightFn);

float fbm(float x, float y, int octaves = 6);
// Tangentes por acúmulo das derivadas de UV de cada triângulo (aproxima MikkTSpace).
void computeTangents(MeshData& mesh);

}  // namespace scene
