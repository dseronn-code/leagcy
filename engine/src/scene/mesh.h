#pragma once
#include <functional>
#include <vector>

#include <glm/glm.hpp>

namespace scene {

struct Vertex {
  glm::vec3 position;
  glm::vec3 normal;
  glm::vec2 uv;
};

struct MeshData {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
};

MeshData makeSphere(uint32_t segments = 96, uint32_t rings = 48);
MeshData makeCube();
// Grade de `res`x`res` quads cobrindo `size` metros; altura dada por heightFn(x, z).
MeshData makeTerrain(float size, uint32_t res, const std::function<float(float, float)>& heightFn);

float fbm(float x, float y, int octaves = 6);

}  // namespace scene
