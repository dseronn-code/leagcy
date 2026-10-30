#include "scene/mesh.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

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

namespace {
struct Quadric {  // erro quádrico: soma de (n·p + d)² dos planos dos triângulos (matriz simétrica 4x4)
  double a[10] = {};
  void addPlane(glm::dvec3 n, double d, double w) {
    const double q[4] = {n.x, n.y, n.z, d};
    int k = 0;
    for (int r = 0; r < 4; ++r)
      for (int c = r; c < 4; ++c) a[k++] += w * q[r] * q[c];
  }
  // Ponto que minimiza o erro; falso se o sistema é mal condicionado (grupo plano ou numa aresta reta).
  bool solve(glm::dvec3& p) const {
    glm::dmat3 A(a[0], a[1], a[2], a[1], a[4], a[5], a[2], a[5], a[7]);
    const glm::dvec3 b(-a[3], -a[6], -a[8]);
    const double det = glm::determinant(A);
    if (std::abs(det) < 1e-12) return false;
    p = glm::inverse(A) * b;
    return true;
  }
};
}  // namespace

MeshData simplifyMesh(const MeshData& mesh, uint32_t targetTriangles) {
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const Vertex& v : mesh.vertices) {
    lo = glm::min(lo, v.position);
    hi = glm::max(hi, v.position);
  }
  const glm::vec3 ext = glm::max(hi - lo, glm::vec3(1e-6f));
  const uint32_t tris = uint32_t(mesh.indices.size() / 3);
  MeshData best;
  // A resolução da grade que dá ~targetTriangles: busca nas resoluções (triângulos sobreviventes crescem com ela).
  float cellsPerAxis = std::sqrt(float(targetTriangles)) * 0.9f;
  for (int attempt = 0; attempt < 6; ++attempt) {
    const float cell = std::max(std::max(ext.x, ext.y), ext.z) / std::max(cellsPerAxis, 1.0f);
    struct Group {
      Quadric q;
      glm::dvec3 sumP{0.0}, sumN{0.0};
      glm::dvec2 sumUv{0.0};
      glm::dvec4 sumT{0.0};
      double count = 0.0;
    };
    std::unordered_map<uint64_t, uint32_t> groupOf;
    std::vector<Group> groups;
    std::vector<uint32_t> vgroup(mesh.vertices.size());
    for (size_t v = 0; v < mesh.vertices.size(); ++v) {
      const Vertex& vx = mesh.vertices[v];
      const glm::ivec3 c = glm::ivec3(glm::floor((vx.position - lo) / cell));
      // Lado da normal (octante): frente e verso de uma parede fina ficam em grupos diferentes.
      const uint32_t side = (vx.normal.x > 0 ? 1u : 0u) | (vx.normal.y > 0 ? 2u : 0u) | (vx.normal.z > 0 ? 4u : 0u);
      const uint64_t key = (uint64_t(uint32_t(c.x) & 0xFFFFF) << 40) | (uint64_t(uint32_t(c.y) & 0xFFFFF) << 20) |
                           uint64_t(uint32_t(c.z) & 0xFFFFF);
      const uint64_t k2 = key * 8u + side;
      auto it = groupOf.find(k2);
      if (it == groupOf.end()) {
        it = groupOf.emplace(k2, uint32_t(groups.size())).first;
        groups.emplace_back();
      }
      vgroup[v] = it->second;
      Group& g = groups[it->second];
      g.sumP += glm::dvec3(vx.position);
      g.sumN += glm::dvec3(vx.normal);
      g.sumUv += glm::dvec2(vx.uv);
      g.sumT += glm::dvec4(vx.tangent);
      g.count += 1.0;
    }
    for (uint32_t t = 0; t < tris; ++t) {
      const glm::dvec3 a = mesh.vertices[mesh.indices[3 * t]].position, b = mesh.vertices[mesh.indices[3 * t + 1]].position,
                       c = mesh.vertices[mesh.indices[3 * t + 2]].position;
      glm::dvec3 n = glm::cross(b - a, c - a);
      const double area2 = glm::length(n);
      if (area2 <= 0.0) continue;
      n /= area2;
      for (int k = 0; k < 3; ++k) groups[vgroup[mesh.indices[3 * t + k]]].q.addPlane(n, -glm::dot(n, a), area2);
    }
    MeshData out;
    out.vertices.resize(groups.size());
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const Group& g = groups[gi];
      const glm::dvec3 mean = g.sumP / g.count;
      glm::dvec3 p;
      // O ponto ótimo só vale se ficar perto da célula (senão o sistema era quase singular): fica a média.
      if (!g.q.solve(p) || glm::length(p - mean) > double(cell)) p = mean;
      Vertex& v = out.vertices[gi];
      v.position = glm::vec3(p);
      v.normal = glm::length(g.sumN) > 1e-9 ? glm::vec3(glm::normalize(g.sumN)) : glm::vec3(0, 1, 0);
      v.uv = glm::vec2(g.sumUv / g.count);
      const glm::dvec3 tt(g.sumT);
      v.tangent = glm::vec4(glm::length(tt) > 1e-9 ? glm::vec3(glm::normalize(tt)) : glm::vec3(1, 0, 0), g.sumT.w >= 0.0 ? 1.0f : -1.0f);
    }
    std::unordered_set<uint64_t> seen;
    for (uint32_t t = 0; t < tris; ++t) {
      uint32_t a = vgroup[mesh.indices[3 * t]], b = vgroup[mesh.indices[3 * t + 1]], c = vgroup[mesh.indices[3 * t + 2]];
      if (a == b || b == c || a == c) continue;  // triângulo colapsou
      {  // virou do avesso no colapso: a normal geométrica contraria as dos vértices
        const glm::vec3 &pa = out.vertices[a].position, &pb = out.vertices[b].position, &pc = out.vertices[c].position;
        const glm::vec3 vn = out.vertices[a].normal + out.vertices[b].normal + out.vertices[c].normal;
        if (glm::dot(glm::cross(pb - pa, pc - pa), vn) <= 0.0f) continue;
      }
      // Mesmo triângulo duas vezes (mesma orientação): uma basta.
      uint32_t r[3] = {a, b, c};
      const int m0 = int(std::min_element(r, r + 3) - r);
      const uint64_t key = (uint64_t(r[m0]) << 42) ^ (uint64_t(r[(m0 + 1) % 3]) << 21) ^ uint64_t(r[(m0 + 2) % 3]);
      if (!seen.insert(key).second) continue;
      out.indices.insert(out.indices.end(), {a, b, c});
    }
    const uint32_t got = uint32_t(out.indices.size() / 3);
    if (best.indices.empty() || std::abs(int(got) - int(targetTriangles)) < std::abs(int(best.indices.size() / 3) - int(targetTriangles)))
      best = std::move(out);
    if (got > targetTriangles * 1.25f) cellsPerAxis *= 0.75f;
    else if (got < targetTriangles * 0.75f) cellsPerAxis *= 1.3f;
    else break;
  }
  return best;
}

MeshData makeHair(uint32_t strands, uint32_t segments, float scalpRadius, float length, float halfWidth, uint32_t seed) {
  MeshData m;
  uint32_t st = seed * 747796405u + 2891336453u;
  auto rnd = [&]() {
    st = st * 747796405u + 2891336453u;
    uint32_t w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967296.0f;
  };
  m.vertices.reserve(size_t(strands) * (segments + 1) * 2);
  for (uint32_t s = 0; s < strands; ++s) {
    // Raiz: calota de cima (y > −0,2), menos a testa (z > 0,55 e y < 0,5).
    glm::vec3 dir;
    do {
      const float z = 1.0f - 2.0f * rnd(), a = glm::two_pi<float>() * rnd(), r = std::sqrt(std::max(1.0f - z * z, 0.0f));
      dir = glm::vec3(r * std::cos(a), z, r * std::sin(a));
    } while (dir.y < -0.2f || (dir.z > 0.55f && dir.y < 0.5f));
    glm::vec3 p = dir * scalpRadius;
    // Sai do couro quase rente a ele, penteado para trás e para baixo (a direção da gravidade/penteado projetada no
    // plano tangente), com um pouco de volume para fora.
    const glm::vec3 comb = glm::normalize(glm::vec3(0.0f, -0.6f, -1.0f));
    glm::vec3 along = comb - dir * glm::dot(comb, dir);
    if (glm::dot(along, along) < 1e-6f) along = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 t = glm::normalize(glm::normalize(along) + dir * 0.25f);
    const float len = length * (0.75f + 0.5f * rnd()), step = len / float(segments);
    const glm::vec3 side = glm::normalize(glm::cross(t, dir) + glm::vec3(1e-4f, 0.0f, 0.0f));
    const uint32_t base = uint32_t(m.vertices.size());
    const float curl = 0.6f * (rnd() - 0.5f);
    for (uint32_t k = 0; k <= segments; ++k) {
      const float u = float(k) / float(segments), w = halfWidth * (1.0f - 0.7f * u);  // afina na ponta
      // Lado da fita perpendicular ao fio neste ponto; normal = t × lado (o shader recupera o lado por normal × t).
      const glm::vec3 sk = glm::normalize(side - t * glm::dot(side, t) + glm::vec3(1e-6f, 0.0f, 0.0f));
      const glm::vec3 off = glm::normalize(glm::cross(t, sk));
      for (int sd = 0; sd < 2; ++sd)
        m.vertices.push_back({p + sk * (sd ? w : -w), off, {float(sd), u}, {t, w}});
      // Próximo ponto: a gravidade dobra o fio (mais na ponta, que pesa menos para o fio segurar), ondulação, e o fio
      // escorrega pela cabeça em vez de entrar nela.
      t = glm::normalize(t + glm::vec3(0.0f, -0.45f, 0.0f) + side * curl * 0.1f);
      glm::vec3 next = p + t * step;
      const float r = glm::length(next), minR = scalpRadius * (1.02f + 0.04f * u);  // camadas: as pontas por fora
      if (r < minR) next *= minR / r;
      t = glm::normalize(next - p);
      p = next;
    }
    for (uint32_t k = 0; k < segments; ++k) {
      const uint32_t a = base + 2 * k;
      m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
  }
  return m;
}

MeshData makeClothGrid(uint32_t width, uint32_t height, float spacing) {
  MeshData m;
  m.clothWidth = width;
  m.clothHeight = height;
  m.clothSpacing = spacing;
  for (uint32_t y = 0; y < height; ++y)
    for (uint32_t x = 0; x < width; ++x) {
      const glm::vec3 p(float(x) * spacing, -float(y) * spacing, 0.0f);
      m.vertices.push_back({p, {0.0f, 0.0f, 1.0f}, {float(x) / float(width - 1), float(y) / float(height - 1)}, {1.0f, 0.0f, 0.0f, 1.0f}});
    }
  for (uint32_t y = 0; y + 1 < height; ++y)
    for (uint32_t x = 0; x + 1 < width; ++x) {
      const uint32_t a = y * width + x, b = a + 1, c = a + width, d = c + 1;
      m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
    }
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
