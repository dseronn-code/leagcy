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
      m.vertices.push_back({n, n, {u, v}, {}});
    }
  }
  for (uint32_t y = 0; y < rings; ++y) {
    for (uint32_t x = 0; x < segments; ++x) {
      uint32_t a = y * (segments + 1) + x, b = a + segments + 1;
      m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
    }
  }
  computeTangents(m);
  return m;
}

MeshData makeGrassClump(uint32_t blades, float height, uint32_t seed) {
  MeshData m;
  uint32_t st = seed * 747796405u + 2891336453u;
  auto rnd = [&]() {
    st = st * 747796405u + 2891336453u;
    uint32_t w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967296.0f;
  };
  for (uint32_t b = 0; b < blades; ++b) {
    const float a = glm::two_pi<float>() * rnd(), r = 0.12f * std::sqrt(rnd());
    const glm::vec3 root(r * std::cos(a), 0.0f, r * std::sin(a));
    const float facing = glm::two_pi<float>() * rnd(), lean = 0.25f + 0.45f * rnd(), h = height * (0.6f + 0.6f * rnd());
    const glm::vec3 side(std::cos(facing), 0.0f, std::sin(facing)), forward(-side.z, 0.0f, side.x);
    const uint32_t base = uint32_t(m.vertices.size());
    for (int k = 0; k <= 3; ++k) {  // 3 segmentos: largura 1,2 cm na base até a ponta
      const float t = float(k) / 3.0f, width = 0.012f * (1.0f - t);
      const glm::vec3 c = root + glm::vec3(0.0f, h * t, 0.0f) + forward * (lean * h * t * t);
      const glm::vec3 n = glm::normalize(glm::cross(side, glm::normalize(glm::vec3(0.0f, h, 0.0f) + forward * (2.0f * lean * h * t))));
      m.vertices.push_back({c - side * width, n, {0.0f, t}, {side, 1.0f}});
      m.vertices.push_back({c + side * width, n, {1.0f, t}, {side, 1.0f}});
    }
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t v = base + 2 * k;
      m.indices.insert(m.indices.end(), {v, v + 1, v + 2, v + 1, v + 3, v + 2});
    }
  }
  return m;
}

MeshData makeRadialGrid(uint32_t rings, uint32_t segments, float innerRadius, float outerRadius) {
  MeshData m;
  m.vertices.push_back({{0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}});
  for (uint32_t r = 0; r < rings; ++r) {
    const float radius = innerRadius * std::pow(outerRadius / innerRadius, float(r) / float(rings - 1));
    for (uint32_t k = 0; k < segments; ++k) {
      const float a = glm::two_pi<float>() * float(k) / float(segments);
      glm::vec3 p(radius * std::cos(a), 0.0f, radius * std::sin(a));
      m.vertices.push_back({p, {0.0f, 1.0f, 0.0f}, {p.x, p.z}, {1.0f, 0.0f, 0.0f, 1.0f}});
    }
  }
  auto ring = [&](uint32_t r, uint32_t k) { return 1u + r * segments + k % segments; };
  // Frente para cima (+y): com o ângulo crescendo de +x para +z, o triângulo (centro, k+1, k) é anti-horário visto
  // de cima (cross((k+1) - centro, k - centro) aponta para +y).
  for (uint32_t k = 0; k < segments; ++k) m.indices.insert(m.indices.end(), {0u, ring(0, k + 1), ring(0, k)});
  for (uint32_t r = 0; r + 1 < rings; ++r)
    for (uint32_t k = 0; k < segments; ++k) {
      const uint32_t a = ring(r, k), b = ring(r, k + 1), c = ring(r + 1, k), d = ring(r + 1, k + 1);
      m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
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
      m.vertices.push_back({n + u * c[0] + v * c[1], n, {(c[0] + 1) * 0.5f, (c[1] + 1) * 0.5f}, {}});
    uint32_t o = f * 4;
    m.indices.insert(m.indices.end(), {o, o + 1, o + 2, o, o + 2, o + 3});
  }
  computeTangents(m);
  return m;
}

MeshData makeTerrain(float size, uint32_t res, const std::function<float(float, float)>& h) {
  MeshData m;
  float step = size / res, half = size * 0.5f, e = step;
  for (uint32_t j = 0; j <= res; ++j) {
    for (uint32_t i = 0; i <= res; ++i) {
      float x = -half + i * step, z = -half + j * step;
      glm::vec3 n = glm::normalize(glm::vec3(h(x - e, z) - h(x + e, z), 2 * e, h(x, z - e) - h(x, z + e)));
      m.vertices.push_back({{x, h(x, z), z}, n, {x, z}, {}});  // UV em metros (para texturas repetidas)
    }
  }
  for (uint32_t j = 0; j < res; ++j) {
    for (uint32_t i = 0; i < res; ++i) {
      uint32_t a = j * (res + 1) + i, b = a + res + 1;
      m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  }
  computeTangents(m);
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

void computeNormals(MeshData& m) {
  for (auto& v : m.vertices) v.normal = glm::vec3(0.0f);
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    Vertex &a = m.vertices[m.indices[i]], &b = m.vertices[m.indices[i + 1]], &c = m.vertices[m.indices[i + 2]];
    glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);  // comprimento = 2 x área
    a.normal += n;
    b.normal += n;
    c.normal += n;
  }
  for (auto& v : m.vertices) v.normal = glm::dot(v.normal, v.normal) > 0.0f ? glm::normalize(v.normal) : glm::vec3(0, 1, 0);
}

void computeTangents(MeshData& m) {
  std::vector<glm::vec3> tan(m.vertices.size(), glm::vec3(0.0f)), bit(m.vertices.size(), glm::vec3(0.0f));
  for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
    uint32_t i0 = m.indices[i], i1 = m.indices[i + 1], i2 = m.indices[i + 2];
    const Vertex &a = m.vertices[i0], &b = m.vertices[i1], &c = m.vertices[i2];
    glm::vec3 e1 = b.position - a.position, e2 = c.position - a.position;
    glm::vec2 d1 = b.uv - a.uv, d2 = c.uv - a.uv;
    float det = d1.x * d2.y - d2.x * d1.y;
    if (std::abs(det) < 1e-12f) continue;
    float r = 1.0f / det;
    glm::vec3 t = (e1 * d2.y - e2 * d1.y) * r;
    glm::vec3 bt = (e2 * d1.x - e1 * d2.x) * r;
    for (uint32_t idx : {i0, i1, i2}) { tan[idx] += t; bit[idx] += bt; }
  }
  for (size_t i = 0; i < m.vertices.size(); ++i) {
    glm::vec3 n = m.vertices[i].normal;
    glm::vec3 t = tan[i] - n * glm::dot(n, tan[i]);  // Gram-Schmidt
    if (glm::dot(t, t) < 1e-12f) {
      t = std::abs(n.x) < 0.9f ? glm::cross(n, glm::vec3(1, 0, 0)) : glm::cross(n, glm::vec3(0, 1, 0));
    }
    t = glm::normalize(t);
    float w = glm::dot(glm::cross(n, t), bit[i]) < 0.0f ? -1.0f : 1.0f;
    m.vertices[i].tangent = glm::vec4(t, w);
  }
}

}  // namespace scene
