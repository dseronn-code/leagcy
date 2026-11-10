#include "game/node_material.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

namespace game {
namespace {

using nlohmann::json;

// Hash inteiro (o mesmo resultado em qualquer sistema) e ruídos periódicos: o período em células é a escala, então a
// textura repete sem emenda.
float hash2(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ seed * 0x9e3779b9u;
  h ^= h >> 15;
  h *= 0x2c1b3c6du;
  h ^= h >> 12;
  h *= 0x297a2d39u;
  h ^= h >> 15;
  return float(h & 0xFFFFFFu) / float(0xFFFFFF);
}
int wrap(int v, int p) { return ((v % p) + p) % p; }

float valueNoise(glm::vec2 p, int period, uint32_t seed) {
  const glm::vec2 f = glm::floor(p), t = p - f, u = t * t * (3.0f - 2.0f * t);
  const int x = int(f.x), y = int(f.y);
  auto h = [&](int a, int b) { return hash2(wrap(a, period), wrap(b, period), seed); };
  const float a = h(x, y), b = h(x + 1, y), c = h(x, y + 1), d = h(x + 1, y + 1);
  return a + (b - a) * u.x + (c - a) * u.y + (a - b - c + d) * u.x * u.y;
}

float fbm(glm::vec2 uv, int scale, int octaves, float gain, uint32_t seed) {
  float sum = 0.0f, amp = 0.5f, norm = 0.0f;
  int s = std::max(scale, 1);
  for (int o = 0; o < octaves; ++o) {
    sum += amp * valueNoise(uv * float(s), s, seed + uint32_t(o) * 101u);
    norm += amp;
    amp *= gain;
    s *= 2;
  }
  return sum / std::max(norm, 1e-6f);
}

// Voronoi periódico: distância ao ponto mais próximo (F1) e à borda (F2 - F1), normalizadas pelo tamanho da célula.
glm::vec2 voronoi(glm::vec2 uv, int scale, uint32_t seed) {
  const int s = std::max(scale, 1);
  const glm::vec2 p = uv * float(s), cell = glm::floor(p);
  float f1 = 1e9f, f2 = 1e9f;
  for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
      const int cx = int(cell.x) + i, cy = int(cell.y) + j;
      const glm::vec2 jitter(hash2(wrap(cx, s), wrap(cy, s), seed), hash2(wrap(cx, s), wrap(cy, s), seed + 7u));
      const float d = glm::length(glm::vec2(float(cx), float(cy)) + jitter - p);
      if (d < f1) {
        f2 = f1;
        f1 = d;
      } else if (d < f2) {
        f2 = d;
      }
    }
  return {f1, f2 - f1};
}

using Value = glm::vec4;  // escalar em .x (e repetido em rgb), cor em rgb

struct Graph {
  std::map<int, json> nodes;
  std::string error;
  // Memória por texel (o grafo é pequeno: um mapa por avaliação basta).
  Value eval(const json& in, glm::vec2 uv, std::map<int, Value>& memo, std::set<int>& stack) {
    if (in.is_number()) {
      const float v = in.get<float>();
      return Value(v, v, v, 1.0f);
    }
    if (in.is_array() && (in.size() == 3 || in.size() == 4))
      return Value(in[0].get<float>(), in[1].get<float>(), in[2].get<float>(), in.size() == 4 ? in[3].get<float>() : 1.0f);
    if (in.is_object() && in.contains("no")) return node(in["no"].get<int>(), uv, memo, stack);
    if (in.is_string() && in.get<std::string>().rfind("#", 0) == 0 && in.get<std::string>().size() == 7) {  // #rrggbb
      const unsigned long c = std::stoul(in.get<std::string>().substr(1), nullptr, 16);
      auto lin = [](float s) { return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f); };
      return Value(lin(float((c >> 16) & 255) / 255.0f), lin(float((c >> 8) & 255) / 255.0f), lin(float(c & 255) / 255.0f), 1.0f);
    }
    error = "entrada inválida: " + in.dump();
    return Value(0.0f);
  }
  Value input(const json& n, const char* k, glm::vec2 uv, std::map<int, Value>& memo, std::set<int>& stack, float def) {
    return n.contains(k) ? eval(n[k], uv, memo, stack) : Value(def, def, def, 1.0f);
  }
  Value node(int id, glm::vec2 uv, std::map<int, Value>& memo, std::set<int>& stack) {
    auto m = memo.find(id);
    if (m != memo.end()) return m->second;
    auto it = nodes.find(id);
    if (it == nodes.end()) {
      error = "referência ao nó " + std::to_string(id) + ", que não existe";
      return Value(0.0f);
    }
    if (stack.count(id)) {
      error = "ciclo no grafo do material passando pelo nó " + std::to_string(id);
      return Value(0.0f);
    }
    stack.insert(id);
    const json& n = it->second;
    const std::string t = n.value("tipo", "");
    const uint32_t seed = n.value("semente", 1u);
    const int scale = n.value("escala", 4);
    Value r(0.0f);
    auto s = [](float v) { return Value(v, v, v, 1.0f); };
    if (t == "ruido") {
      r = s(fbm(uv, scale, n.value("oitavas", 5), n.value("ganho", 0.5f), seed));
    } else if (t == "voronoi") {
      const glm::vec2 v = voronoi(uv, scale, seed);
      r = s(n.value("saida", "distancia") == "borda" ? std::min(v.y * 2.0f, 1.0f) : std::min(v.x, 1.0f));
    } else if (t == "tijolos") {  // 1 no tijolo, 0 na argamassa (borda suave); fileiras alternadas com meio deslocamento
      const int rows = std::max(n.value("linhas", 8), 1), cols = std::max(n.value("colunas", 4), 1);
      const float mortar = n.value("argamassa", 0.06f);
      const float y = uv.y * float(rows), row = std::floor(y);
      const float x = uv.x * float(cols) + (int(row) % 2 ? 0.5f : 0.0f);
      const float fx = x - std::floor(x), fy = y - row;
      const float dx = std::min(fx, 1.0f - fx) * float(rows) / float(cols), dy = std::min(fy, 1.0f - fy);
      const float d = std::min(dx, dy);  // distância à borda em unidades de altura de fileira
      r = s(glm::smoothstep(mortar * 0.5f, mortar, d));
    } else if (t == "listras") {
      const float f = uv.x * float(scale) + n.value("inclinacao", 0.0f) * uv.y * float(scale);
      r = s(0.5f + 0.5f * std::sin(6.2831853f * f));
    } else if (t == "gradiente") {
      r = s(n.value("eixo", "v") == "u" ? uv.x : uv.y);
    } else if (t == "constante") {
      r = input(n, "valor", uv, memo, stack, 0.0f);
    } else if (t == "misturar") {
      const Value a = input(n, "a", uv, memo, stack, 0.0f), b = input(n, "b", uv, memo, stack, 1.0f);
      const float k = glm::clamp(input(n, "t", uv, memo, stack, 0.5f).x, 0.0f, 1.0f);
      r = a + (b - a) * k;
    } else if (t == "multiplicar") {
      r = input(n, "a", uv, memo, stack, 1.0f) * input(n, "b", uv, memo, stack, 1.0f);
    } else if (t == "somar") {
      r = input(n, "a", uv, memo, stack, 0.0f) + input(n, "b", uv, memo, stack, 0.0f);
    } else if (t == "inverter") {
      r = Value(1.0f) - input(n, "entrada", uv, memo, stack, 0.0f);
    } else if (t == "contraste") {  // smoothstep(min, max, entrada)
      const float v = input(n, "entrada", uv, memo, stack, 0.0f).x;
      r = s(glm::smoothstep(n.value("min", 0.3f), n.value("max", 0.7f), v));
    } else if (t == "rampa") {  // "cores": [[posição, cor], ...] ordenadas pela posição
      const float v = glm::clamp(input(n, "entrada", uv, memo, stack, 0.0f).x, 0.0f, 1.0f);
      const json& cs = n.value("cores", json::array());
      if (cs.empty()) {
        error = "rampa sem \"cores\"";
      } else {
        Value prev = eval(cs[0][1], uv, memo, stack);
        float prevPos = cs[0][0].get<float>();
        r = prev;
        for (size_t i = 1; i < cs.size(); ++i) {
          const float pos = cs[i][0].get<float>();
          const Value c = eval(cs[i][1], uv, memo, stack);
          if (v <= pos) {
            const float k = pos > prevPos ? (v - prevPos) / (pos - prevPos) : 1.0f;
            r = v <= prevPos ? prev : prev + (c - prev) * k;
            break;
          }
          r = c;
          prev = c;
          prevPos = pos;
        }
      }
    } else if (t == "deformar") {  // desloca a UV pela entrada (ruído) antes de avaliar outro nó
      const float amt = n.value("forca", 0.05f);
      const glm::vec2 d(input(n, "x", uv, memo, stack, 0.5f).x - 0.5f, input(n, "y", uv, memo, stack, 0.5f).x - 0.5f);
      std::map<int, Value> memo2;
      r = eval(n.value("entrada", json(0)), uv + d * amt, memo2, stack);
    } else {
      error = "nó " + std::to_string(id) + ": tipo desconhecido '" + t + "'";
    }
    stack.erase(id);
    memo[id] = r;
    return r;
  }
};

uint8_t toByte(float v) { return uint8_t(glm::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
float toSrgb(float l) {
  l = glm::clamp(l, 0.0f, 1.0f);
  return l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

}  // namespace

bool bakeNodeMaterial(const json& graph, uint32_t size, NodeMaterialBake& out, std::string& error) {
  Graph g;
  for (const json& n : graph.value("nos", json::array())) {
    if (!n.contains("id") || !n["id"].is_number_integer()) {
      error = "nó sem \"id\" inteiro: " + n.dump();
      return false;
    }
    g.nodes[n["id"].get<int>()] = n;
  }
  if (!graph.contains("saida") || !graph["saida"].is_object()) {
    error = "o material por nós precisa de \"saida\" (cor, rugosidade, metalico, altura)";
    return false;
  }
  const json& sOut = graph["saida"];
  const bool hasHeight = sOut.contains("altura");
  // Uma avaliação de teste (erros do grafo aparecem antes do laço).
  {
    std::map<int, Value> memo;
    std::set<int> stack;
    for (const char* k : {"cor", "rugosidade", "metalico", "altura"})
      if (sOut.contains(k)) g.eval(sOut[k], glm::vec2(0.5f), memo, stack);
    if (!g.error.empty()) {
      error = g.error;
      return false;
    }
  }
  const size_t N = size_t(size) * size;
  std::vector<glm::vec3> color(N);
  std::vector<float> rough(N), metal(N), height(hasHeight ? N : 0);
  auto work = [&](uint32_t y0, uint32_t y1) {
    Graph local = g;  // cada thread com o seu (sem estado compartilhado)
    for (uint32_t y = y0; y < y1; ++y)
      for (uint32_t x = 0; x < size; ++x) {
        const glm::vec2 uv((float(x) + 0.5f) / float(size), (float(y) + 0.5f) / float(size));
        std::map<int, Value> memo;
        std::set<int> stack;
        const size_t i = size_t(y) * size + x;
        color[i] = sOut.contains("cor") ? glm::vec3(local.eval(sOut["cor"], uv, memo, stack)) : glm::vec3(0.8f);
        rough[i] = sOut.contains("rugosidade") ? local.eval(sOut["rugosidade"], uv, memo, stack).x : 0.5f;
        metal[i] = sOut.contains("metalico") ? local.eval(sOut["metalico"], uv, memo, stack).x : 0.0f;
        if (hasHeight) height[i] = local.eval(sOut["altura"], uv, memo, stack).x;
      }
  };
  const uint32_t threads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  for (uint32_t t = 0; t < threads; ++t)
    pool.emplace_back(work, size * t / threads, size * (t + 1) / threads);
  for (std::thread& th : pool) th.join();

  auto image = [&](bool srgb, bool normal) {
    scene::TextureData d;
    d.width = d.height = size;
    d.srgb = srgb;
    d.normalMap = normal;
    d.rgba.resize(N * 4);
    return d;
  };
  out.color = image(true, false);
  out.metalRough = image(false, false);
  for (size_t i = 0; i < N; ++i) {
    out.color.rgba[i * 4 + 0] = toByte(toSrgb(color[i].r));
    out.color.rgba[i * 4 + 1] = toByte(toSrgb(color[i].g));
    out.color.rgba[i * 4 + 2] = toByte(toSrgb(color[i].b));
    out.color.rgba[i * 4 + 3] = 255;
    out.metalRough.rgba[i * 4 + 0] = 255;  // oclusão (não usada aqui)
    out.metalRough.rgba[i * 4 + 1] = toByte(rough[i]);  // glTF: G = rugosidade, B = metal
    out.metalRough.rgba[i * 4 + 2] = toByte(metal[i]);
    out.metalRough.rgba[i * 4 + 3] = 255;
  }
  out.color.name = "material por nós (cor)";
  out.metalRough.name = "material por nós (rugosidade/metal)";
  out.hasNormal = hasHeight;
  if (hasHeight) {
    // Normal do relevo (diferenças centrais com repetição). "forca_relevo" = altura em unidades de UV por 1,0 do relevo.
    const float strength = graph["saida"].value("forca_relevo", 0.02f) * float(size);
    out.normal = image(false, true);
    for (uint32_t y = 0; y < size; ++y)
      for (uint32_t x = 0; x < size; ++x) {
        auto h = [&](int a, int b) { return height[size_t(wrap(b, int(size))) * size + size_t(wrap(a, int(size)))]; };
        const float dx = (h(int(x) + 1, int(y)) - h(int(x) - 1, int(y))) * 0.5f * strength;
        const float dy = (h(int(x), int(y) + 1) - h(int(x), int(y) - 1)) * 0.5f * strength;
        const glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
        const size_t i = size_t(y) * size + x;
        out.normal.rgba[i * 4 + 0] = toByte(n.x * 0.5f + 0.5f);
        out.normal.rgba[i * 4 + 1] = toByte(n.y * 0.5f + 0.5f);
        out.normal.rgba[i * 4 + 2] = toByte(n.z * 0.5f + 0.5f);
        out.normal.rgba[i * 4 + 3] = 255;
      }
    out.normal.name = "material por nós (normal)";
  }
  return true;
}

}  // namespace game
