#include "scene/mesh.h"
#include "core/carregamento.h"

#ifdef LEGACY_MIKKTSPACE
#include <mikktspace.h>
#endif

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <cstddef>
#include <atomic>
#include <thread>

#include <glm/gtc/constants.hpp>
#include <meshoptimizer.h>

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

MeshData simplifyMesh(const MeshData& mesh, uint32_t targetTriangles, std::vector<uint32_t>* groupsOut) {
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
      glm::dvec4 sumT{0.0}, sumC{0.0};
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
      if (!mesh.colors.empty()) g.sumC += glm::dvec4(mesh.colors[v]);
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
    if (!mesh.colors.empty()) {
      out.colors.resize(groups.size());
      for (size_t gi = 0; gi < groups.size(); ++gi) out.colors[gi] = glm::vec4(groups[gi].sumC / groups[gi].count);
    }
    for (size_t gi = 0; gi < groups.size(); ++gi) {
      const Group& g = groups[gi];
      const glm::dvec3 mean = g.sumP / g.count;
      glm::dvec3 p;
      // O ponto ótimo só vale se ficar perto da célula (senão o sistema era quase singular): fica a média.
      if (groupsOut || !g.q.solve(p) || glm::length(p - mean) > double(cell)) p = mean;
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
    if (best.indices.empty() || std::abs(int(got) - int(targetTriangles)) < std::abs(int(best.indices.size() / 3) - int(targetTriangles))) {
      best = std::move(out);
      if (groupsOut) *groupsOut = vgroup;
    }
    if (got > targetTriangles * 1.25f) cellsPerAxis *= 0.75f;
    else if (got < targetTriangles * 0.75f) cellsPerAxis *= 1.3f;
    else break;
  }
  return best;
}

MeshData applyVertexGroups(const MeshData& pose, const std::vector<uint32_t>& groups, const MeshData& lod) {
  MeshData out;
  out.indices = lod.indices;
  out.vertices.assign(lod.vertices.size(), Vertex{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec2(0.0f), glm::vec4(0.0f)});
  std::vector<float> count(lod.vertices.size(), 0.0f);
  for (size_t v = 0; v < pose.vertices.size() && v < groups.size(); ++v) {
    Vertex& o = out.vertices[groups[v]];
    o.position += pose.vertices[v].position;
    o.normal += pose.vertices[v].normal;
    o.uv += pose.vertices[v].uv;
    o.tangent += pose.vertices[v].tangent;
    count[groups[v]] += 1.0f;
  }
  for (size_t g = 0; g < out.vertices.size(); ++g) {
    Vertex& o = out.vertices[g];
    const float c = std::max(count[g], 1.0f);
    o.position /= c;
    o.uv /= c;
    o.normal = glm::length(o.normal) > 1e-9f ? glm::normalize(o.normal) : glm::vec3(0, 1, 0);
    const glm::vec3 t(o.tangent);
    o.tangent = glm::vec4(glm::length(t) > 1e-9f ? glm::normalize(t) : glm::vec3(1, 0, 0), o.tangent.w >= 0.0f ? 1.0f : -1.0f);
  }
  return out;
}

MeshData makeHair(uint32_t strands, uint32_t segments, float scalpRadius, float length, float halfWidth, uint32_t seed) {
  // Penteado por guias (como os grooms de cinema e de jogos): ~1 guia para cada 40 fios define a forma de uma mecha
  // (direção, ondulação, comprimento); cada fio nasce perto da raiz da sua guia, segue o caminho dela e se junta a
  // ela perto da ponta (clumping), com um pouco de frizz próprio. Fios independentes viravam uma penugem rala.
  MeshData m;
  uint32_t st = seed * 747796405u + 2891336453u;
  auto rnd = [&]() {
    st = st * 747796405u + 2891336453u;
    uint32_t w = ((st >> ((st >> 28u) + 4u)) ^ st) * 277803737u;
    return float((w >> 22u) ^ w) / 4294967296.0f;
  };
  // Raiz no couro: calota de cima, sem a testa (linha do cabelo em arco) nem o rosto; costeletas e nuca ficam.
  auto raiz = [&]() {
    glm::vec3 dir;
    for (;;) {
      const float z = 1.0f - 2.0f * rnd(), a = glm::two_pi<float>() * rnd(), r = std::sqrt(std::max(1.0f - z * z, 0.0f));
      dir = glm::vec3(r * std::cos(a), z, r * std::sin(a));
      const float linhaTesta = 0.32f + 0.25f * dir.x * dir.x;  // a linha sobe nas têmporas
      if (dir.y < -0.35f) continue;                              // abaixo da nuca
      if (dir.z > 0.35f && dir.y < linhaTesta) continue;         // testa e rosto
      if (std::abs(dir.x) > 0.75f && dir.z > 0.2f && dir.y < -0.05f) continue;  // bochecha (costeleta curta acima)
      return dir;
    }
  };
  const uint32_t guides = std::max(8u, strands / 40u), pts = segments + 1;
  std::vector<glm::vec3> G(size_t(guides) * pts);
  std::vector<glm::vec3> Groot(guides);
  // Repartido de lado (x ≈ 0,25): as guias dos dois lados tombam para lados opostos a partir da risca.
  const float risca = 0.25f;
  for (uint32_t g = 0; g < guides; ++g) {
    const glm::vec3 dir = raiz();
    Groot[g] = dir;
    glm::vec3 p = dir * scalpRadius;
    const float lado = dir.x > risca ? 1.0f : -1.0f;
    const glm::vec3 comb = glm::normalize(glm::vec3(0.55f * lado, -0.7f, -0.6f));  // penteado: para o lado e para trás
    glm::vec3 along = comb - dir * glm::dot(comb, dir);
    if (glm::dot(along, along) < 1e-6f) along = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 t = glm::normalize(glm::normalize(along) + dir * 0.35f);              // volume na raiz
    const float len = length * (0.8f + 0.4f * rnd()) * (dir.y < 0.0f ? 0.85f : 1.0f), step = len / float(segments);
    const glm::vec3 side = glm::normalize(glm::cross(t, dir) + glm::vec3(1e-4f, 0.0f, 0.0f));
    const float onda = 0.06f + 0.1f * rnd(), fase = glm::two_pi<float>() * rnd(), freq = 2.0f + 1.5f * rnd();
    for (uint32_t k = 0; k < pts; ++k) {
      const float u = float(k) / float(segments);
      G[size_t(g) * pts + k] = p + side * (onda * std::sin(freq * glm::pi<float>() * u + fase) * u * 0.5f);
      t = glm::normalize(t + glm::vec3(0.0f, -0.4f, 0.0f) * (0.5f + u));
      glm::vec3 next = p + t * step;
      const float r = glm::length(next), minR = scalpRadius * (1.02f + 0.05f * u);  // camadas: pontas por fora
      if (r < minR) next *= minR / r;
      t = glm::normalize(next - p);
      p = next;
    }
  }
  m.vertices.reserve(size_t(strands) * pts * 2);
  for (uint32_t s = 0; s < strands; ++s) {
    const uint32_t g = std::min(guides - 1, uint32_t(rnd() * float(guides)));
    // Raiz do fio: perto da raiz da guia (no couro), deslocada no plano tangente
    const glm::vec3 gd = Groot[g];
    const glm::vec3 e1 = glm::normalize(glm::cross(gd, std::abs(gd.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
    const glm::vec3 e2 = glm::cross(gd, e1);
    const float ang = glm::two_pi<float>() * rnd(), rad = 0.07f * std::sqrt(rnd());
    const glm::vec3 off0 = (e1 * std::cos(ang) + e2 * std::sin(ang)) * rad * scalpRadius;
    const float clump = 0.55f + 0.35f * rnd(), frizz = 0.004f + 0.01f * rnd() * rnd();
    const float fz1 = glm::two_pi<float>() * rnd(), fz2 = glm::two_pi<float>() * rnd();
    std::vector<glm::vec3> P(pts);
    for (uint32_t k = 0; k < pts; ++k) {
      const float u = float(k) / float(segments);
      glm::vec3 q = G[size_t(g) * pts + k] + off0 * (1.0f - clump * u * u);
      q += scalpRadius * frizz * u * glm::vec3(std::sin(17.0f * u + fz1), std::sin(23.0f * u + fz2), std::cos(19.0f * u + fz1));
      const float r = glm::length(q), minR = scalpRadius * (1.015f + 0.04f * u);
      if (r < minR) q *= minR / r;
      P[k] = q;
    }
    const uint32_t base = uint32_t(m.vertices.size());
    for (uint32_t k = 0; k < pts; ++k) {
      const float u = float(k) / float(segments), w = halfWidth * (1.0f - 0.75f * u);  // afina na ponta
      const glm::vec3 t = glm::normalize(P[std::min(k + 1, pts - 1)] - P[k > 0 ? k - 1 : 0] + glm::vec3(1e-7f));
      const glm::vec3 radial = glm::normalize(P[k] + glm::vec3(1e-7f));
      const glm::vec3 sk = glm::normalize(glm::cross(t, radial) + glm::vec3(1e-6f, 0.0f, 0.0f));
      const glm::vec3 nrm = glm::normalize(glm::cross(t, sk));
      for (int sd = 0; sd < 2; ++sd) m.vertices.push_back({P[k] + sk * (sd ? w : -w), nrm, {float(sd), u}, {t, w}});
    }
    for (uint32_t k = 0; k < segments; ++k) {
      const uint32_t a = base + 2 * k;
      m.indices.insert(m.indices.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
  }
  m.hairStrands = strands;
  m.hairPoints = pts;
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

MeshData makeClothSheet(uint32_t width, uint32_t height, float spacing) {
  MeshData m = makeClothGrid(width, height, spacing);
  m.clothPinned = false;
  const glm::vec2 c(0.5f * float(width - 1) * spacing, 0.5f * float(height - 1) * spacing);
  for (Vertex& v : m.vertices) {  // xy (pendurado) -> xz (deitado), centrado
    v.position = glm::vec3(v.position.x - c.x, 0.0f, -v.position.y - c.y);
    v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
  }
  for (size_t k = 0; k + 2 < m.indices.size(); k += 3) std::swap(m.indices[k + 1], m.indices[k + 2]);  // frente para cima
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
    // glTF: bitangente = normal × tangente × w apontando para v DECRESCENTE (origem das UVs em cima; o normal map
    // segue a convenção do OpenGL, verde para cima). bit[] é a derivada em +v: w é o oposto do sinal dela. Medido no
    // NormalTangentMirrorTest da Khronos: com o sinal ao contrário, 100% dos vértices discordavam das tangentes que o
    // Blender exporta e o relevo saía invertido nos modelos sem TANGENT (DamagedHelmet, o scan de pele).
    float w = glm::dot(glm::cross(n, t), bit[i]) < 0.0f ? 1.0f : -1.0f;
    m.vertices[i].tangent = glm::vec4(t, w);
  }
}

#ifdef LEGACY_MIKKTSPACE
namespace {
struct MikkContext {
  const MeshData* mesh;
  std::vector<glm::vec4> corner;  // tangente de cada canto (3 por triângulo)
};
int mikkFaces(const SMikkTSpaceContext* c) { return int(static_cast<MikkContext*>(c->m_pUserData)->mesh->indices.size() / 3); }
int mikkFaceVerts(const SMikkTSpaceContext*, int) { return 3; }
const Vertex& mikkVertex(const SMikkTSpaceContext* c, int face, int vert) {
  const MeshData& m = *static_cast<MikkContext*>(c->m_pUserData)->mesh;
  return m.vertices[m.indices[size_t(face) * 3 + size_t(vert)]];
}
void mikkPosition(const SMikkTSpaceContext* c, float out[], int face, int vert) {
  const glm::vec3 p = mikkVertex(c, face, vert).position;
  out[0] = p.x, out[1] = p.y, out[2] = p.z;
}
void mikkNormal(const SMikkTSpaceContext* c, float out[], int face, int vert) {
  const glm::vec3 n = mikkVertex(c, face, vert).normal;
  out[0] = n.x, out[1] = n.y, out[2] = n.z;
}
void mikkUV(const SMikkTSpaceContext* c, float out[], int face, int vert) {
  // glTF tem a origem das UVs em cima; o MikkTSpace (como o Blender) conta de baixo: v invertido (o sinal da
  // bitangente sai igual ao que o exportador do Blender grava).
  const glm::vec2 uv = mikkVertex(c, face, vert).uv;
  out[0] = uv.x, out[1] = 1.0f - uv.y;
}
void mikkSet(const SMikkTSpaceContext* c, const float t[], float sign, int face, int vert) {
  static_cast<MikkContext*>(c->m_pUserData)->corner[size_t(face) * 3 + size_t(vert)] = glm::vec4(t[0], t[1], t[2], sign);
}
}  // namespace
#endif

namespace {
std::string g_tangentCacheDir = carregamento::pastaCache();
std::mutex g_tangentCacheMutex;

uint64_t fnvBytes(const void* p, size_t n, uint64_t h) {
  const uint8_t* b = static_cast<const uint8_t*>(p);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}

// Arquivo do cache: "LTAN" + versão + número de cantos + os cantos (vec4 por índice).
constexpr uint32_t kTangentCacheVersion = 1;

bool loadTangentCache(const std::filesystem::path& f, std::vector<glm::vec4>& corner) {
  std::ifstream in(f, std::ios::binary);
  if (!in) return false;
  uint32_t head[3] = {};
  in.read(reinterpret_cast<char*>(head), sizeof(head));
  if (!in || head[0] != 0x4e41544cu || head[1] != kTangentCacheVersion || head[2] != corner.size()) return false;
  in.read(reinterpret_cast<char*>(corner.data()), std::streamsize(corner.size() * sizeof(glm::vec4)));
  return bool(in);
}

void saveTangentCache(const std::filesystem::path& f, const std::vector<glm::vec4>& corner) {
  std::error_code ec;
  std::filesystem::create_directories(f.parent_path(), ec);
  std::filesystem::path tmp = f;
  tmp += ".tmp" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()));
  {
    std::ofstream out(tmp, std::ios::binary);
    if (!out) return;
    const uint32_t head[3] = {0x4e41544cu, kTangentCacheVersion, uint32_t(corner.size())};
    out.write(reinterpret_cast<const char*>(head), sizeof(head));
    out.write(reinterpret_cast<const char*>(corner.data()), std::streamsize(corner.size() * sizeof(glm::vec4)));
    if (!out) return;
  }
  std::filesystem::rename(tmp, f, ec);  // atômico: um cache pela metade nunca é lido
  if (ec) std::filesystem::remove(tmp, ec);
}
}  // namespace

void setTangentCacheDir(const std::string& dir) {
  std::lock_guard<std::mutex> lock(g_tangentCacheMutex);
  g_tangentCacheDir = dir;
}

void computeTangentsMikk(MeshData& m, std::vector<uint32_t>* origin) {
  if (origin) {
    origin->resize(m.vertices.size());
    for (uint32_t i = 0; i < origin->size(); ++i) (*origin)[i] = i;
  }
#ifndef LEGACY_MIKKTSPACE
  computeTangents(m);  // compilado sem a biblioteca (LEGACY_MIKKTSPACE=OFF): a aproximação
}
#else
  if (m.indices.size() < 3) return;
  MikkContext ctx{&m, std::vector<glm::vec4>(m.indices.size() / 3 * 3, glm::vec4(0.0f))};
  // Cache pelo conteúdo (posição, normal e UV de cada canto usado): o MikkTSpace é lento (minutos numa cidade inteira).
  std::string cacheDir;
  {
    std::lock_guard<std::mutex> lock(g_tangentCacheMutex);
    cacheDir = g_tangentCacheDir;
  }
  std::filesystem::path cacheFile;
  if (!cacheDir.empty() && ctx.corner.size() >= 3000) {
    uint64_t h = 1469598103934665603ull;
    h = fnvBytes(&kTangentCacheVersion, sizeof(kTangentCacheVersion), h);
    for (const Vertex& v : m.vertices) h = fnvBytes(&v, offsetof(Vertex, tangent), h);  // sem a tangente (lixo/padrão)
    h = fnvBytes(m.indices.data(), ctx.corner.size() * sizeof(uint32_t), h);
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.tan", static_cast<unsigned long long>(h));
    cacheFile = std::filesystem::path(cacheDir) / name;
    carregamento::cacheUsado(name);
  }
  if (cacheFile.empty() || !loadTangentCache(cacheFile, ctx.corner)) {
    SMikkTSpaceInterface in{};
    in.m_getNumFaces = mikkFaces;
    in.m_getNumVerticesOfFace = mikkFaceVerts;
    in.m_getPosition = mikkPosition;
    in.m_getNormal = mikkNormal;
    in.m_getTexCoord = mikkUV;
    in.m_setTSpaceBasic = mikkSet;
    SMikkTSpaceContext c{&in, &ctx};
    if (!genTangSpaceDefault(&c)) {  // sem memória: fica a aproximação
      computeTangents(m);
      return;
    }
    if (!cacheFile.empty()) saveTangentCache(cacheFile, ctx.corner);
  }
  // Primeiro canto de cada vértice define a tangente dele; um canto que discorda ganha uma cópia do vértice (as
  // cópias com a mesma tangente são reaproveitadas).
  const size_t original = m.vertices.size();
  std::vector<int32_t> first(original, -1);
  std::vector<std::vector<uint32_t>> copies(original);
  auto same = [](const glm::vec4& a, const glm::vec4& b) {
    return a.w == b.w && glm::dot(glm::vec3(a), glm::vec3(b)) > 0.99999f;
  };
  for (size_t k = 0; k < ctx.corner.size(); ++k) {
    const uint32_t v = m.indices[k];
    const glm::vec4 t = ctx.corner[k];
    if (first[v] < 0) {
      first[v] = int32_t(v);
      m.vertices[v].tangent = t;
      continue;
    }
    if (same(m.vertices[v].tangent, t)) continue;
    uint32_t use = ~0u;
    for (uint32_t cp : copies[v])
      if (same(m.vertices[cp].tangent, t)) use = cp;
    if (use == ~0u) {
      use = uint32_t(m.vertices.size());
      Vertex x = m.vertices[v];
      x.tangent = t;
      m.vertices.push_back(x);
      if (!m.colors.empty()) m.colors.push_back(m.colors[v]);
      if (!m.joints.empty()) m.joints.push_back(m.joints[v]);
      if (!m.weights.empty()) m.weights.push_back(m.weights[v]);
      if (origin) origin->push_back(v);
      copies[v].push_back(use);
    }
    m.indices[k] = use;
  }
}
#endif

namespace {
// Um meshlet a partir de `count` triângulos (índices de vértice em `tri`, 3 por triângulo): esfera, cone das normais e
// os triângulos com índices locais. `local` é um rascunho do tamanho dos vértices, todo -1 (volta assim).
void appendMeshlet(MeshletData& out, const MeshData& mesh, const uint32_t* tri, uint32_t count, uint32_t firstTriangle,
                   std::vector<int32_t>& local) {
  if (count == 0) return;
  std::vector<uint32_t> verts;
  Meshlet m{};
  m.vertexOffset = uint32_t(out.vertices.size());
  m.triangleOffset = uint32_t(out.triangles.size());
  m.firstTriangle = firstTriangle;
  for (uint32_t k = 0; k < 3 * count; ++k)
    if (local[tri[k]] < 0) {
      local[tri[k]] = int32_t(verts.size());
      verts.push_back(tri[k]);
    }
  m.counts = uint32_t(verts.size()) | (count << 8);
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (uint32_t v : verts) {
    lo = glm::min(lo, mesh.vertices[v].position);
    hi = glm::max(hi, mesh.vertices[v].position);
  }
  m.center = 0.5f * (lo + hi);
  for (uint32_t v : verts) m.radius = std::max(m.radius, glm::length(mesh.vertices[v].position - m.center));
  // Cone das normais das faces (pela ordem dos vértices, como o rasterizador decide frente e verso).
  glm::vec3 sum(0.0f);
  std::vector<glm::vec3> normals;
  for (uint32_t t = 0; t < count; ++t) {
    const glm::vec3 a = mesh.vertices[tri[3 * t]].position, b = mesh.vertices[tri[3 * t + 1]].position,
                    c = mesh.vertices[tri[3 * t + 2]].position;
    glm::vec3 n = glm::cross(b - a, c - a);
    const float l = glm::length(n);
    if (l > 0.0f) {
      normals.push_back(n / l);
      sum += n / l;
    }
    const uint32_t i0 = uint32_t(local[tri[3 * t]]), i1 = uint32_t(local[tri[3 * t + 1]]), i2 = uint32_t(local[tri[3 * t + 2]]);
    out.triangles.push_back(i0 | (i1 << 8) | (i2 << 16));
  }
  m.coneCutoff = 1.0f;
  const float sl = glm::length(sum);
  if (sl > 1e-6f && !normals.empty()) {
    m.coneAxis = sum / sl;
    float minDot = 1.0f;
    for (const glm::vec3& n : normals) minDot = std::min(minDot, glm::dot(n, m.coneAxis));
    // Cone aberto demais (mais de ~84° de meio-ângulo): nunca fica todo de costas.
    if (minDot > 0.1f) m.coneCutoff = std::sqrt(1.0f - minDot * minDot);
  }
  out.vertices.insert(out.vertices.end(), verts.begin(), verts.end());
  for (uint32_t v : verts) local[v] = -1;
  out.meshlets.push_back(m);
}
}  // namespace

MeshletData buildMeshlets(const MeshData& mesh) {
  MeshletData out;
  const uint32_t triCount = uint32_t(mesh.indices.size() / 3);
  std::vector<int32_t> local(mesh.vertices.size(), -1);
  std::unordered_set<uint32_t> verts;
  uint32_t first = 0;
  for (uint32_t t = 0; t < triCount; ++t) {
    uint32_t extra = 0;
    for (int k = 0; k < 3; ++k) {
      const uint32_t v = mesh.indices[3 * t + k];
      bool repeated = verts.count(v) > 0;
      for (int j = 0; j < k; ++j) repeated = repeated || mesh.indices[3 * t + j] == v;
      extra += repeated ? 0 : 1;
    }
    if (verts.size() + extra > kMeshletMaxVertices || t - first + 1 > kMeshletMaxTriangles) {
      appendMeshlet(out, mesh, &mesh.indices[3 * first], t - first, first, local);
      first = t;
      verts.clear();
    }
    for (int k = 0; k < 3; ++k) verts.insert(mesh.indices[3 * t + k]);
  }
  appendMeshlet(out, mesh, mesh.indices.data() + 3 * first, triCount - first, first, local);
  return out;
}

namespace {
struct LodSphere {
  glm::vec3 center{0.0f};
  float radius = 0.0f;
  float error = 0.0f;
};
struct LodCluster {
  std::vector<uint32_t> indices;  // 3 por triângulo
  LodSphere self, parent;
  bool base = false;              // nível 0 (o meshlet já existe em `out`)
  uint32_t baseMeshlet = 0;
  uint32_t level = 0;
};

// Esfera de um grupo: contém as esferas de todos os membros; erro = o maior deles (monotonia do grafo).
LodSphere mergeSpheres(const std::vector<LodCluster>& cl, const std::vector<uint32_t>& group) {
  LodSphere r;
  float w = 0.0f;
  for (uint32_t c : group) {
    r.center += cl[c].self.center * std::max(cl[c].self.radius, 1e-6f);
    w += std::max(cl[c].self.radius, 1e-6f);
  }
  r.center /= w;
  for (uint32_t c : group) {
    r.radius = std::max(r.radius, cl[c].self.radius + glm::length(cl[c].self.center - r.center));
    r.error = std::max(r.error, cl[c].self.error);
  }
  return r;
}

// Grupos de até `size` meshlets vizinhos (que dividem vértices, pela posição): começa por um meshlet ainda sem grupo e
// vai puxando o vizinho que mais divide vértices com o grupo. Sem vizinho livre, o grupo fica menor.
std::vector<std::vector<uint32_t>> groupClusters(const std::vector<LodCluster>& cl, const std::vector<uint32_t>& pending,
                                                 const std::vector<uint32_t>& remap, size_t size) {
  std::unordered_map<uint32_t, std::vector<uint32_t>> byVertex;  // vértice (pela posição) -> posições em `pending`
  for (uint32_t p = 0; p < pending.size(); ++p)
    for (uint32_t v : cl[pending[p]].indices) {
      std::vector<uint32_t>& l = byVertex[remap[v]];
      if (l.empty() || l.back() != p) l.push_back(p);
    }
  std::vector<std::unordered_map<uint32_t, uint32_t>> adj(pending.size());  // vizinho -> vértices em comum
  for (const auto& [v, l] : byVertex)
    for (size_t a = 0; a < l.size(); ++a)
      for (size_t b = a + 1; b < l.size(); ++b) {
        ++adj[l[a]][l[b]];
        ++adj[l[b]][l[a]];
      }
  std::vector<int32_t> groupOf(pending.size(), -1);
  std::vector<std::vector<uint32_t>> groups;
  for (uint32_t seed = 0; seed < pending.size(); ++seed) {
    if (groupOf[seed] >= 0) continue;
    const int32_t g = int32_t(groups.size());
    groups.push_back({seed});
    groupOf[seed] = g;
    std::unordered_map<uint32_t, uint32_t> frontier;  // candidato -> vértices em comum com o grupo
    for (const auto& [n, w] : adj[seed])
      if (groupOf[n] < 0) frontier[n] += w;
    while (groups[size_t(g)].size() < size && !frontier.empty()) {
      auto best = frontier.begin();
      for (auto it = frontier.begin(); it != frontier.end(); ++it)
        if (it->second > best->second || (it->second == best->second && it->first < best->first)) best = it;
      const uint32_t pick = best->first;
      frontier.erase(best);
      groupOf[pick] = g;
      groups[size_t(g)].push_back(pick);
      for (const auto& [n, w] : adj[pick])
        if (groupOf[n] < 0) frontier[n] += w;
    }
  }
  // Pedaços que ficaram sozinhos (sem vizinho livre: ilhas, ou cercados por grupos já cheios) se juntam aos sozinhos
  // mais próximos no espaço; sozinhos eles nunca simplificariam e a raiz do grafo ficaria grande.
  std::vector<uint32_t> alone;
  std::vector<std::vector<uint32_t>> kept;
  for (auto& gr : groups) {
    if (gr.size() == 1) alone.push_back(gr[0]);
    else kept.push_back(std::move(gr));
  }
  std::vector<uint8_t> used(alone.size(), 0);
  for (size_t a = 0; a < alone.size(); ++a) {
    if (used[a]) continue;
    used[a] = 1;
    std::vector<uint32_t> g{alone[a]};
    const glm::vec3 c = cl[pending[alone[a]]].self.center;
    std::vector<std::pair<float, size_t>> near;
    for (size_t b = a + 1; b < alone.size(); ++b)
      if (!used[b]) near.push_back({glm::length(cl[pending[alone[b]]].self.center - c), b});
    std::sort(near.begin(), near.end());
    for (size_t k = 0; k < near.size() && g.size() < size; ++k) {
      used[near[k].second] = 1;
      g.push_back(alone[near[k].second]);
    }
    kept.push_back(std::move(g));
  }
  groups = std::move(kept);
  for (auto& gr : groups)
    for (uint32_t& p : gr) p = pending[p];
  return groups;
}

// Meshlets (≤ 64 vértices, ≤ 124 triângulos) de uma lista de triângulos qualquer, pela meshoptimizer.
std::vector<std::vector<uint32_t>> splitClusters(const MeshData& mesh, const std::vector<uint32_t>& indices) {
  const size_t maxM = meshopt_buildMeshletsBound(indices.size(), kMeshletMaxVertices, kMeshletMaxTriangles);
  std::vector<meshopt_Meshlet> ms(maxM);
  std::vector<unsigned int> mv(maxM * kMeshletMaxVertices);
  std::vector<unsigned char> mt(maxM * kMeshletMaxTriangles * 3);
  ms.resize(meshopt_buildMeshlets(ms.data(), mv.data(), mt.data(), indices.data(), indices.size(),
                                  &mesh.vertices[0].position.x, mesh.vertices.size(), sizeof(Vertex), kMeshletMaxVertices,
                                  kMeshletMaxTriangles, 0.0f));
  std::vector<std::vector<uint32_t>> out(ms.size());
  for (size_t i = 0; i < ms.size(); ++i) {
    out[i].resize(ms[i].triangle_count * 3);
    for (size_t k = 0; k < ms[i].triangle_count * 3; ++k)
      out[i][k] = mv[ms[i].vertex_offset + mt[ms[i].triangle_offset + k]];
  }
  return out;
}
}  // namespace

void reorderForClusters(MeshData& mesh) {
  if (mesh.indices.size() < 3) return;
  const std::vector<std::vector<uint32_t>> parts = splitClusters(mesh, mesh.indices);
  std::vector<uint32_t> reordered;
  reordered.reserve(mesh.indices.size());
  mesh.clusterStarts.clear();
  for (const std::vector<uint32_t>& p : parts) {
    mesh.clusterStarts.push_back(uint32_t(reordered.size() / 3));
    reordered.insert(reordered.end(), p.begin(), p.end());
  }
  if (reordered.size() != mesh.indices.size()) {  // não deveria acontecer: fica a ordem original
    mesh.clusterStarts.clear();
    return;
  }
  mesh.indices = std::move(reordered);
}

constexpr double kMinAreaKept = 0.8;  // fração mínima da área "para fora" que um grupo simplificado mantém

MeshletData buildClusterLod(const MeshData& mesh, std::vector<MeshletLod>& lods, ClusterLodStats* stats) {
  constexpr size_t kGroup = 8;
  // Nível 0: os pedaços compactos de reorderForClusters (contínuos na lista de índices; os triângulos mantêm o número)
  // ou, sem eles, os trechos contínuos de buildMeshlets (finos em malhas em faixas: simplificam mal).
  MeshletData out;
  if (!mesh.clusterStarts.empty()) {
    std::vector<int32_t> scratch(mesh.vertices.size(), -1);
    const uint32_t triCount = uint32_t(mesh.indices.size() / 3);
    for (size_t k = 0; k < mesh.clusterStarts.size(); ++k) {
      const uint32_t a = mesh.clusterStarts[k], b = k + 1 < mesh.clusterStarts.size() ? mesh.clusterStarts[k + 1] : triCount;
      appendMeshlet(out, mesh, &mesh.indices[3 * size_t(a)], b - a, a, scratch);
    }
  } else {
    out = buildMeshlets(mesh);
  }
  std::vector<LodCluster> cl(out.meshlets.size());
  for (size_t i = 0; i < cl.size(); ++i) {
    const Meshlet& m = out.meshlets[i];
    const uint32_t n = m.counts >> 8;
    cl[i].indices.assign(mesh.indices.begin() + 3 * m.firstTriangle, mesh.indices.begin() + 3 * (m.firstTriangle + n));
    cl[i].self = {m.center, m.radius, 0.0f};
    cl[i].parent.error = std::numeric_limits<float>::infinity();
    cl[i].base = true;
    cl[i].baseMeshlet = uint32_t(i);
  }
  // Vértices na mesma posição contam como o mesmo (costuras de UV e normal): travados juntos, sem abrir buracos.
  std::vector<uint32_t> remap(mesh.vertices.size());
  const meshopt_Stream pos = {&mesh.vertices[0].position.x, sizeof(float) * 3, sizeof(Vertex)};
  meshopt_generateVertexRemapMulti(remap.data(), nullptr, mesh.vertices.size(), mesh.vertices.size(), &pos, 1);
  std::vector<uint32_t> pending(cl.size());
  for (uint32_t i = 0; i < pending.size(); ++i) pending[i] = i;
  std::vector<unsigned char> locks(mesh.vertices.size());
  uint32_t levels = 1;
  while (pending.size() > 1) {
    // No fim (até 4 grupos de pedaços), todos num grupo só: sem borda entre grupos, nada fica travado e a raiz encolhe.
    const std::vector<std::vector<uint32_t>> groups =
        pending.size() <= 4 * kGroup ? std::vector<std::vector<uint32_t>>{pending} : groupClusters(cl, pending, remap, kGroup);
    // Trava os vértices na borda entre grupos (o mesmo vértice em dois grupos): os vizinhos continuam encaixando.
    std::vector<int32_t> groupOfVertex(mesh.vertices.size(), -1);
    for (size_t g = 0; g < groups.size(); ++g)
      for (uint32_t c : groups[g])
        for (uint32_t v : cl[c].indices) {
          int32_t& o = groupOfVertex[remap[v]];
          o = (o == -1 || o == int32_t(g)) ? int32_t(g) : -2;
        }
    for (size_t v = 0; v < locks.size(); ++v) locks[v] = groupOfVertex[remap[v]] == -2;
    pending.clear();
    std::vector<uint32_t> retry;
    size_t made = 0, stuck = 0, shrunk = 0;
    for (const std::vector<uint32_t>& group : groups) {
      if (group.size() == 1) {  // sem vizinho livre: tenta de novo no próximo nível
        retry.push_back(group[0]);
        stuck += cl[group[0]].indices.size() / 3;
        continue;
      }
      std::vector<uint32_t> merged;
      for (uint32_t c : group) merged.insert(merged.end(), cl[c].indices.begin(), cl[c].indices.end());
      const size_t target = (merged.size() / 6) * 3;  // metade dos triângulos (os pedaços nem sempre estão cheios)
      std::vector<uint32_t> simplified(merged.size());
      float error = 0.0f;
      // Só pela geometria: o erro devolvido é então a distância (unidades da malha) que a GPU projeta na tela. Com peso
      // em normais e UV ele misturava o desvio desses atributos e passava do tamanho da própria rocha (a GPU nunca
      // escolhia os níveis grossos: 8 milhões de triângulos para uma tela de 480x270). As costuras de UV e normal
      // continuam presas: a meshoptimizer acha vértices na mesma posição com atributos diferentes sozinha.
      simplified.resize(meshopt_simplifyWithAttributes(simplified.data(), merged.data(), merged.size(),
                                                       &mesh.vertices[0].position.x, mesh.vertices.size(), sizeof(Vertex),
                                                       nullptr, 0, nullptr, 0, locks.data(), target, FLT_MAX,
                                                       meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute, &error));
      // Sem reduzir o bastante (borda travada demais): o grupo fica para o próximo nível.
      if (simplified.size() > merged.size() * 0.85f ||
          simplified.size() / (kMeshletMaxTriangles * 3) >= merged.size() / (kMeshletMaxTriangles * 3)) {
        for (uint32_t c : group) retry.push_back(c);
        stuck += merged.size() / 3;
        continue;
      }
      // Cobertura: a área "virada para fora" (cada triângulo projetado na normal da superfície; dobras se cancelam)
      // tem que continuar quase a mesma. A simplificação às vezes dobra a superfície para dentro com um erro que parece
      // pequeno (visto no Windows: o último grupo da rocha perdeu 59% da área); o pai não cobriria mais os filhos e o
      // corte do grafo abriria um buraco. Nesse caso o grupo fica para o próximo nível, como um travado.
      auto outwardArea = [&](const std::vector<uint32_t>& ix) {
        double sum = 0.0;
        for (size_t t = 0; t + 2 < ix.size(); t += 3) {
          const Vertex &va = mesh.vertices[ix[t]], &vb = mesh.vertices[ix[t + 1]], &vc = mesh.vertices[ix[t + 2]];
          const glm::vec3 n = va.normal + vb.normal + vc.normal;
          const float nl = glm::length(n);
          if (nl > 0.0f) sum += 0.5 * glm::dot(glm::cross(vb.position - va.position, vc.position - va.position), n / nl);
        }
        return sum;
      };
      const double areaBefore = outwardArea(merged), areaAfter = outwardArea(simplified);
      if (std::getenv("LEGACY_DEBUG_LOD") && (error < 1e-5f || areaAfter < kMinAreaKept * areaBefore))
        std::fprintf(stderr, "nivel %u: grupo de %zu pedaços: %zu -> %zu tri, área para fora %.4f -> %.4f (%.3f), erro %.7f\n",
                     levels, group.size(), merged.size() / 3, simplified.size() / 3, areaBefore, areaAfter,
                     areaBefore > 0.0 ? areaAfter / areaBefore : 0.0, error);
      if (simplified.empty() || areaAfter < kMinAreaKept * areaBefore) {
        for (uint32_t c : group) retry.push_back(c);
        stuck += merged.size() / 3;
        ++shrunk;
        continue;
      }
      LodSphere g = mergeSpheres(cl, group);
      g.error += error;  // conservador: o erro da simplificação soma ao dos filhos
      for (uint32_t c : group) cl[c].parent = g;
      for (std::vector<uint32_t>& part : splitClusters(mesh, simplified)) {
        LodCluster n;
        n.indices = std::move(part);
        n.self = g;
        n.level = levels;
        n.parent.error = std::numeric_limits<float>::infinity();
        made += n.indices.size() / 3;
        pending.push_back(uint32_t(cl.size()));
        cl.push_back(std::move(n));
      }
    }
    if (std::getenv("LEGACY_DEBUG_LOD"))  // diagnóstico: um resumo por nível
      std::fprintf(stderr, "nivel %u: %zu grupos, feitos %zu tri, travados %zu tri (%zu grupos encolheram), pendentes %zu, retry %zu\n",
                   levels, groups.size(), made, stuck, shrunk, pending.size(), retry.size());
    if (made == 0) break;
    ++levels;
    if (made < stuck / 3) break;  // quase tudo travado: parar aqui
    pending.insert(pending.end(), retry.begin(), retry.end());
  }
  // Saída: o nível 0 já está em `out`; os outros viram meshlets novos (sem triângulo na lista original).
  std::vector<int32_t> local(mesh.vertices.size(), -1);
  lods.assign(out.meshlets.size(), MeshletLod{});
  ClusterLodStats st;
  st.baseMeshlets = uint32_t(out.meshlets.size());
  for (const LodCluster& c : cl) {
    MeshletLod l{};
    l.self = glm::vec4(c.self.center, c.self.radius);
    l.parent = glm::vec4(c.parent.center, c.parent.radius);
    l.selfError = c.self.error;
    l.parentError = std::isinf(c.parent.error) ? FLT_MAX : c.parent.error;
    if (c.base) {
      lods[c.baseMeshlet] = l;
      st.baseTriangles += c.indices.size() / 3;
    } else {
      // A simplificação às vezes vira um triângulo do avesso (dobra; ~1% nos níveis grossos da rocha): com o descarte
      // de faces de trás ele some e abre um buraco (visto na RTX). Virado para o lado das normais da superfície, ele
      // volta a aparecer; a forma é a mesma (o ray tracing e a interseção exata não dependem da ordem).
      std::vector<uint32_t> idx = c.indices;
      for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const Vertex &a = mesh.vertices[idx[t]], &b = mesh.vertices[idx[t + 1]], &d = mesh.vertices[idx[t + 2]];
        if (glm::dot(glm::cross(b.position - a.position, d.position - a.position), a.normal + b.normal + d.normal) < 0.0f) {
          std::swap(idx[t + 1], idx[t + 2]);
          ++st.flippedTriangles;
        }
      }
      appendMeshlet(out, mesh, idx.data(), uint32_t(idx.size() / 3), kNoTriangle, local);
      lods.push_back(l);
    }
    if (std::isinf(c.parent.error)) st.rootTriangles += c.indices.size() / 3;
  }
  st.levels = levels;
  st.totalMeshlets = uint32_t(out.meshlets.size());
  // Ordem final: por nível e, dentro dele, pela posição (curva de Morton). Os blocos de 32 do task shader ficam de um
  // nível só e de uma região pequena: buildMeshletBlocks consegue descartá-los inteiros.
  std::vector<uint32_t> level(out.meshlets.size(), 0);
  {
    size_t k = st.baseMeshlets;
    for (const LodCluster& c : cl)
      if (!c.base) level[k++] = c.level;
  }
  glm::vec3 lo(1e30f), hi(-1e30f);
  for (const Meshlet& m : out.meshlets) {
    lo = glm::min(lo, m.center);
    hi = glm::max(hi, m.center);
  }
  const glm::vec3 ext = glm::max(hi - lo, glm::vec3(1e-6f));
  auto morton = [&](glm::vec3 p) {
    const glm::uvec3 q = glm::uvec3(glm::clamp((p - lo) / ext, 0.0f, 1.0f) * 1023.0f);
    auto spread = [](uint32_t x) {
      x = (x | (x << 16)) & 0x030000FFu;
      x = (x | (x << 8)) & 0x0300F00Fu;
      x = (x | (x << 4)) & 0x030C30C3u;
      return (x | (x << 2)) & 0x09249249u;
    };
    return spread(q.x) | (spread(q.y) << 1) | (spread(q.z) << 2);
  };
  std::vector<uint32_t> order(out.meshlets.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  std::vector<uint32_t> code(out.meshlets.size());
  for (size_t i = 0; i < code.size(); ++i) code[i] = morton(out.meshlets[i].center);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return level[a] != level[b] ? level[a] < level[b] : code[a] < code[b];
  });
  std::vector<Meshlet> sortedMeshlets(order.size());
  std::vector<MeshletLod> sortedLods(order.size());
  for (size_t i = 0; i < order.size(); ++i) {
    sortedMeshlets[i] = out.meshlets[order[i]];
    sortedLods[i] = lods[order[i]];
  }
  out.meshlets = std::move(sortedMeshlets);
  lods = std::move(sortedLods);
  if (stats) *stats = st;
  return out;
}

std::vector<MeshletBlock> buildMeshletBlocks(const std::vector<MeshletLod>& lods) {
  std::vector<MeshletBlock> blocks((lods.size() + kMeshletBlock - 1) / kMeshletBlock);
  auto unite = [](const MeshletLod* l, size_t n, bool parent) {
    glm::vec3 c(0.0f);
    for (size_t i = 0; i < n; ++i) c += glm::vec3(parent ? l[i].parent : l[i].self);
    c /= float(n);
    float r = 0.0f;
    for (size_t i = 0; i < n; ++i) {
      const glm::vec4 s = parent ? l[i].parent : l[i].self;
      r = std::max(r, glm::length(glm::vec3(s) - c) + s.w);
    }
    return glm::vec4(c, r);
  };
  for (size_t b = 0; b < blocks.size(); ++b) {
    const size_t first = b * kMeshletBlock, n = std::min<size_t>(kMeshletBlock, lods.size() - first);
    MeshletBlock& k = blocks[b];
    k.self = unite(&lods[first], n, false);
    k.minSelfError = FLT_MAX;
    k.maxParentError = 0.0f;
    bool anyRoot = false;
    for (size_t i = first; i < first + n; ++i) {
      k.minSelfError = std::min(k.minSelfError, lods[i].selfError);
      if (lods[i].parentError >= FLT_MAX) anyRoot = true;
      else k.maxParentError = std::max(k.maxParentError, lods[i].parentError);
    }
    if (anyRoot) k.maxParentError = FLT_MAX;  // a raiz nunca é fina demais
    k.parent = anyRoot ? k.self : unite(&lods[first], n, true);
  }
  return blocks;
}

namespace {
constexpr uint32_t kClusterLodCacheVersion = 5;  // mude quando o algoritmo do grafo mudar
uint64_t fnv1aBytes(const void* data, size_t n, uint64_t h) {
  const uint8_t* b = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
  return h;
}
template <class T>
bool readVec(std::ifstream& in, std::vector<T>& v, uint64_t count) {
  v.resize(size_t(count));
  in.read(reinterpret_cast<char*>(v.data()), std::streamsize(count * sizeof(T)));
  return bool(in);
}
template <class T>
void writeVec(std::ofstream& out, const std::vector<T>& v) {
  out.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * sizeof(T)));
}
}  // namespace

MeshletData buildClusterLodCached(const MeshData& mesh, std::vector<MeshletLod>& lods, ClusterLodStats* stats,
                                  const std::string& cacheDir) {
  if (cacheDir.empty()) return buildClusterLod(mesh, lods, stats);
  uint64_t h = 1469598103934665603ull;
  const uint32_t key[3] = {kClusterLodCacheVersion, kMeshletMaxVertices, kMeshletMaxTriangles};
  h = fnv1aBytes(key, sizeof(key), h);
  h = fnv1aBytes(mesh.vertices.data(), mesh.vertices.size() * sizeof(Vertex), h);
  h = fnv1aBytes(mesh.indices.data(), mesh.indices.size() * sizeof(uint32_t), h);
  h = fnv1aBytes(mesh.clusterStarts.data(), mesh.clusterStarts.size() * sizeof(uint32_t), h);
  char name[40];
  std::snprintf(name, sizeof(name), "%016llx.lod", static_cast<unsigned long long>(h));
  const std::filesystem::path file = std::filesystem::path(cacheDir) / name;
  carregamento::cacheUsado(name);
  struct Header {
    char magic[4];
    uint32_t version;
    ClusterLodStats st;
    uint64_t meshlets, vertices, triangles;
  };
  {
    std::ifstream in(file, std::ios::binary);
    Header hd{};
    if (in && in.read(reinterpret_cast<char*>(&hd), sizeof(hd)) && std::memcmp(hd.magic, "LGLD", 4) == 0 &&
        hd.version == kClusterLodCacheVersion) {
      MeshletData out;
      if (readVec(in, out.meshlets, hd.meshlets) && readVec(in, out.vertices, hd.vertices) &&
          readVec(in, out.triangles, hd.triangles) && readVec(in, lods, hd.meshlets)) {
        if (stats) *stats = hd.st;
        return out;
      }
    }
  }
  ClusterLodStats st;
  MeshletData out = buildClusterLod(mesh, lods, &st);
  if (stats) *stats = st;
  std::error_code ec;
  std::filesystem::create_directories(cacheDir, ec);
  const std::filesystem::path tmp = file.string() + ".tmp";  // escreve inteiro e renomeia: nunca um arquivo pela metade
  {
    std::ofstream o(tmp, std::ios::binary);
    Header hd{{'L', 'G', 'L', 'D'}, kClusterLodCacheVersion, st, out.meshlets.size(), out.vertices.size(), out.triangles.size()};
    o.write(reinterpret_cast<const char*>(&hd), sizeof(hd));
    writeVec(o, out.meshlets);
    writeVec(o, out.vertices);
    writeVec(o, out.triangles);
    writeVec(o, lods);
  }
  std::filesystem::rename(tmp, file, ec);
  return out;
}

}  // namespace scene
