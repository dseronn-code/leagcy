#include "scene/mesh.h"

#include <cmath>

#include <glm/gtc/constants.hpp>

namespace scene {

MeshData makeSphere(uint32_t segments, uint32_t rings) {
  MeshData m;
  for (uint32_t y = 0; y <= rings; ++y) {
    float v = float(y) / rings, theta = v * glm::pi<float>();
    for (uint32_t x = 0; x <= segments; ++x) {
      float u = float(x) / segments, phi = u * glm::two_pi<float>();
      glm::vec3 n(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
      m.vertices.push_back({n, n, {u, v}});
    }
  }
  for (uint32_t y = 0; y < rings; ++y) {
    for (uint32_t x = 0; x < segments; ++x) {
      uint32_t a = y * (segments + 1) + x, b = a + segments + 1;
      m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
  }
  return m;
}

MeshData makeCube() {
  // normal, eixo U, eixo V de cada face (U x V = normal => winding anti-horário)
  const glm::vec3 faces[6][3] = {
      {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
      {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}}, {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
      {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
  };
  const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  MeshData m;
  for (uint32_t f = 0; f < 6; ++f) {
    auto [n, u, v] = faces[f];
    for (auto& c : corners)
      m.vertices.push_back({n + u * c[0] + v * c[1], n, {(c[0] + 1) * 0.5f, (c[1] + 1) * 0.5f}});
    uint32_t o = f * 4;
    m.indices.insert(m.indices.end(), {o, o + 1, o + 2, o, o + 2, o + 3});
  }
  return m;
}

MeshData makeTerrain(float size, uint32_t res, const std::function<float(float, float)>& h) {
  MeshData m;
  float step = size / res, half = size * 0.5f, e = step;
  for (uint32_t j = 0; j <= res; ++j) {
    for (uint32_t i = 0; i <= res; ++i) {
      float x = -half + i * step, z = -half + j * step;
      glm::vec3 n = glm::normalize(glm::vec3(h(x - e, z) - h(x + e, z), 2 * e, h(x, z - e) - h(x, z + e)));
      m.vertices.push_back({{x, h(x, z), z}, n, {x, z}});  // UV em metros (para texturas repetidas)
    }
  }
  for (uint32_t j = 0; j < res; ++j) {
    for (uint32_t i = 0; i < res; ++i) {
      uint32_t a = j * (res + 1) + i, b = a + res + 1;
      m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  return m;
}

namespace {
float hash(float x, float y) {
  float s = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
  return s - std::floor(s);
}
float valueNoise(float x, float y) {
  float xi = std::floor(x), yi = std::floor(y), xf = x - xi, yf = y - yi;
  float u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
  float a = hash(xi, yi), b = hash(xi + 1, yi), c = hash(xi, yi + 1), d = hash(xi + 1, yi + 1);
  return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
}  // namespace

float fbm(float x, float y, int octaves) {
  float sum = 0, amp = 0.5f, freq = 1;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * valueNoise(x * freq, y * freq);
    freq *= 2.03f;
    amp *= 0.5f;
  }
  return sum;
}

}  // namespace scene
