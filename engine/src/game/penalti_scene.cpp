// Estádio do Pênalti da Treta: noite, quatro torres de refletores (luz de malha de verdade, amostrada pelo ReSTIR),
// arquibancadas com torcida (multidão assada), placas de LED que iluminam o gramado, gol com a rede de tecido
// simulado (a bola é um colisor), gramado com textura e tufos de grama 3D, a bola e os dois jogadores com esqueleto.
#include "game/penalti.h"
#include "game/partida.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/log.h"
#include "core/paths.h"
#include <nlohmann/json.hpp>
#include "stb_easy_font.h"

namespace game {

using namespace scene;

PenaltiWorld& world() {
  static PenaltiWorld w;
  return w;
}

namespace {

glm::mat4 trs(glm::vec3 t, glm::vec3 s, float yawRad = 0.0f) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
  m = glm::rotate(m, yawRad, glm::vec3(0, 1, 0));
  return glm::scale(m, s);
}

// Cilindro de raio 1 entre y = -1 e y = 1 (como o cubo de makeCube), com tampas.
MeshData makeCylinder(uint32_t segments) {
  MeshData m;
  for (uint32_t i = 0; i <= segments; ++i) {
    const float u = float(i) / float(segments), a = u * glm::two_pi<float>();
    const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
    const glm::vec4 t(-std::sin(a), 0.0f, std::cos(a), 1.0f);
    m.vertices.push_back({n + glm::vec3(0, -1, 0), n, {u, 1.0f}, t});
    m.vertices.push_back({n + glm::vec3(0, 1, 0), n, {u, 0.0f}, t});
  }
  for (uint32_t i = 0; i < segments; ++i) {
    const uint32_t a = 2 * i, b = a + 1, c = a + 2, d = a + 3;
    m.indices.insert(m.indices.end(), {a, b, c, c, b, d});
  }
  for (int side = -1; side <= 1; side += 2) {
    const uint32_t center = uint32_t(m.vertices.size());
    const glm::vec3 n(0.0f, float(side), 0.0f);
    m.vertices.push_back({n, n, {0.5f, 0.5f}, {1, 0, 0, 1}});
    for (uint32_t i = 0; i <= segments; ++i) {
      const float a = float(i) / float(segments) * glm::two_pi<float>();
      m.vertices.push_back({glm::vec3(std::cos(a), float(side), std::sin(a)), n, {0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a)}, {1, 0, 0, 1}});
    }
    for (uint32_t i = 0; i < segments; ++i) {
      const uint32_t a = center + 1 + i, b = a + 1;
      if (side > 0) m.indices.insert(m.indices.end(), {center, b, a});
      else m.indices.insert(m.indices.end(), {center, a, b});
    }
  }
  return m;
}

// Quadrado de -1 a 1 no plano XY virado para +z (uv 0..1 com v para baixo).
MeshData makeQuad() {
  MeshData m;
  const glm::vec3 n(0, 0, 1);
  const glm::vec4 t(1, 0, 0, 1);
  m.vertices = {{{-1, -1, 0}, n, {0, 1}, t}, {{1, -1, 0}, n, {1, 1}, t}, {{-1, 1, 0}, n, {0, 0}, t}, {{1, 1, 0}, n, {1, 0}, t}};
  m.indices = {0, 1, 2, 2, 1, 3};
  return m;
}

TextureData blankTexture(int w, int h, glm::u8vec4 c, bool srgb = true) {
  TextureData t;
  t.width = uint32_t(w);
  t.height = uint32_t(h);
  t.srgb = srgb;
  t.rgba.resize(size_t(w) * size_t(h) * 4);
  for (size_t i = 0; i < t.rgba.size(); i += 4) std::memcpy(&t.rgba[i], &c, 4);
  return t;
}

// Bola: 12 gomos pretos (pentágonos nos vértices de um icosaedro) e costuras escuras entre os gomos brancos.
TextureData ballTexture() {
  const int W = 1024, H = 512;
  TextureData t = blankTexture(W, H, {240, 240, 236, 255});
  t.name = "bola";
  const float g = (1.0f + std::sqrt(5.0f)) * 0.5f;
  std::vector<glm::vec3> v = {{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0}, {0, -1, g}, {0, 1, g},
                              {0, -1, -g}, {0, 1, -g}, {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}};
  for (auto& p : v) p = glm::normalize(p);
  for (int y = 0; y < H; ++y) {
    const float theta = (float(y) + 0.5f) / float(H) * glm::pi<float>();
    for (int x = 0; x < W; ++x) {
      const float phi = (float(x) + 0.5f) / float(W) * glm::two_pi<float>();
      const glm::vec3 d(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi));
      float best = -2.0f, second = -2.0f;
      for (const auto& p : v) {
        const float c = glm::dot(d, p);
        if (c > best) { second = best; best = c; } else if (c > second) second = c;
      }
      uint8_t* px = &t.rgba[(size_t(y) * W + size_t(x)) * 4];
      const float ang = std::acos(std::min(best, 1.0f));
      if (ang < 0.33f) { px[0] = px[1] = px[2] = 22; }                 // gomo preto
      else if (best - second < 0.012f) { px[0] = px[1] = px[2] = 120; }  // costura
    }
  }
  return t;
}

// Rede: losangos de fio branco com alfa (recorte), repetida pela escala de UV do material.
TextureData netTexture() {
  const int S = 64;
  TextureData t = blankTexture(S, S, {235, 235, 230, 0});
  t.name = "rede";
  for (int y = 0; y < S; ++y)
    for (int x = 0; x < S; ++x) {
      const float u = float(x) / S, w = float(y) / S;
      const float d1 = std::abs(std::fmod(u + w + 1.0f, 1.0f) - 0.5f), d2 = std::abs(std::fmod(u - w + 1.0f, 1.0f) - 0.5f);
      if (std::max(d1, d2) > 0.44f) t.rgba[(size_t(y) * S + size_t(x)) * 4 + 3] = 255;
    }
  return t;
}

// Marcação do campo (decal de imagem visto de cima): listras do corte da grama, linhas de cal e o gasto na marca do
// pênalti e na pequena área. Cobre x em [-40, 40] e z em [-30, 50].
constexpr float kFieldX0 = -40.0f, kFieldX1 = 40.0f, kFieldZ0 = -30.0f, kFieldZ1 = 50.0f;
TextureData fieldTexture() {
  const int S = 2048;
  TextureData t = blankTexture(S, S, {0, 0, 0, 0});
  t.name = "marcacao";
  auto px = [&](float x, float z) { return glm::ivec2(int((x - kFieldX0) / (kFieldX1 - kFieldX0) * S), int((kFieldZ1 - z) / (kFieldZ1 - kFieldZ0) * S)); };
  auto put = [&](int x, int y, glm::u8vec4 c, float a) {
    if (x < 0 || y < 0 || x >= S || y >= S) return;
    uint8_t* p = &t.rgba[(size_t(y) * S + size_t(x)) * 4];
    const float k = a;
    for (int i = 0; i < 3; ++i) p[i] = uint8_t(float(p[i]) * (1.0f - k) + float(c[i]) * k);
    p[3] = uint8_t(std::max(float(p[3]), 255.0f * a));
  };
  // listras de 4 m (mais escura / mais clara), alfa baixo: só tingem a textura da grama
  for (int y = 0; y < S; ++y) {
    const float z = kFieldZ1 - (float(y) + 0.5f) / S * (kFieldZ1 - kFieldZ0);
    const bool dark = int(std::floor((z + 30.0f) / 4.0f)) % 2 == 0;
    for (int x = 0; x < S; ++x) {
      uint8_t* p = &t.rgba[(size_t(y) * S + size_t(x)) * 4];
      const glm::u8vec4 c = dark ? glm::u8vec4(18, 46, 14, 255) : glm::u8vec4(58, 96, 30, 255);
      p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 34;
    }
  }
  // desgaste (terra) na marca do pênalti e na pequena área
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  auto wear = [&](float cx, float cz, float r, float amount) {
    for (int k = 0; k < int(r * r * 3000.0f); ++k) {
      const float a = u(rng) * glm::two_pi<float>(), d = r * std::sqrt(u(rng));
      const glm::ivec2 p = px(cx + d * std::cos(a), cz + d * std::sin(a) * 0.8f);
      put(p.x, p.y, {88, 70, 42, 255}, amount * (1.0f - d / r) * u(rng));
    }
  };
  wear(0.0f, kSpotZ, 1.3f, 0.9f);
  wear(0.0f, 0.9f, 2.6f, 0.7f);
  wear(-2.4f, 0.5f, 1.2f, 0.5f);
  wear(2.4f, 0.5f, 1.2f, 0.5f);
  // linhas de cal: 12 cm
  const float lw = 0.12f;
  auto line = [&](glm::vec2 a, glm::vec2 b) {
    const float len = glm::length(b - a);
    const int steps = int(len / ((kFieldX1 - kFieldX0) / S) * 2.0f) + 2;
    for (int i = 0; i <= steps; ++i) {
      const glm::vec2 p = glm::mix(a, b, float(i) / float(steps));
      const glm::ivec2 c = px(p.x, p.y);
      const int r = int(lw * 0.5f / ((kFieldX1 - kFieldX0) / S) + 0.5f);
      for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) put(c.x + dx, c.y + dy, {236, 238, 232, 255}, 0.92f);
    }
  };
  line({-40.0f, 0.0f}, {40.0f, 0.0f});                                    // linha de fundo
  line({-20.16f, 0.0f}, {-20.16f, 16.5f}); line({20.16f, 0.0f}, {20.16f, 16.5f}); line({-20.16f, 16.5f}, {20.16f, 16.5f});
  line({-9.16f, 0.0f}, {-9.16f, 5.5f}); line({9.16f, 0.0f}, {9.16f, 5.5f}); line({-9.16f, 5.5f}, {9.16f, 5.5f});
  for (int i = 0; i < 64; ++i) {                                           // meia-lua da grande área
    const float a0 = 0.645f + (glm::pi<float>() - 1.29f) * float(i) / 64.0f, a1 = 0.645f + (glm::pi<float>() - 1.29f) * float(i + 1) / 64.0f;
    line({9.15f * std::cos(a0), kSpotZ + 9.15f * std::sin(a0)}, {9.15f * std::cos(a1), kSpotZ + 9.15f * std::sin(a1)});
  }
  for (int i = 0; i < 24; ++i) {                                           // marca do pênalti (círculo de 22 cm)
    const float a = glm::two_pi<float>() * float(i) / 24.0f;
    line({0.0f, kSpotZ}, {0.11f * std::cos(a), kSpotZ + 0.11f * std::sin(a)});
  }
  return t;
}

// Painel de lâmpadas do refletor: 6 x 4 lâmpadas redondas brilhantes numa placa escura (textura emissiva).
TextureData lampTexture() {
  const int W = 384, H = 256;
  TextureData t = blankTexture(W, H, {8, 8, 9, 255});
  t.name = "refletor";
  for (int ly = 0; ly < 4; ++ly)
    for (int lx = 0; lx < 6; ++lx) {
      const float cx = 32.0f + lx * 64.0f, cy = 32.0f + ly * 64.0f;
      for (int y = int(cy) - 30; y < int(cy) + 30; ++y)
        for (int x = int(cx) - 30; x < int(cx) + 30; ++x) {
          const float d = std::hypot(float(x) - cx, float(y) - cy) / 26.0f;
          if (d > 1.0f) continue;
          const float k = 1.0f - d * d * 0.35f;
          uint8_t* p = &t.rgba[(size_t(y) * W + size_t(x)) * 4];
          p[0] = uint8_t(255 * k); p[1] = uint8_t(246 * k); p[2] = uint8_t(226 * k);
        }
    }
  return t;
}

}  // namespace

void drawText(std::vector<uint8_t>& rgba, int width, int height, const std::string& text, int x0, int y0, int scale,
              glm::u8vec4 color) {
  static char buffer[300000];
  const int quads = stb_easy_font_print(0.0f, 0.0f, const_cast<char*>(text.c_str()), nullptr, buffer, sizeof(buffer));
  const float* v = reinterpret_cast<const float*>(buffer);
  for (int q = 0; q < quads; ++q) {
    const float* a = v + q * 16;  // 4 vértices de (x, y, z, cor) = 16 bytes cada
    const float qx0 = std::min(a[0], a[8]), qx1 = std::max(a[0], a[8]), qy0 = std::min(a[1], a[9]), qy1 = std::max(a[1], a[9]);
    for (int y = int(qy0 * scale); y < int(qy1 * scale); ++y)
      for (int x = int(qx0 * scale); x < int(qx1 * scale); ++x) {
        const int px = x0 + x, py = y0 + y;
        if (px < 0 || py < 0 || px >= width || py >= height) continue;
        std::memcpy(&rgba[(size_t(py) * size_t(width) + size_t(px)) * 4], &color, 4);
      }
  }
}

namespace {

// Placa de LED: texto repetido em âmbar sobre preto, com a grade de pontos de um painel de LED.
TextureData ledTexture(const std::string& text, glm::u8vec4 color) {
  const int W = 2048, H = 96;
  TextureData t = blankTexture(W, H, {3, 3, 4, 255});
  t.name = "led";
  const int scale = 9, step = int(stb_easy_font_width(const_cast<char*>(text.c_str()))) * scale + 90;
  for (int x = 10; x < W; x += std::max(step, 64)) drawText(t.rgba, W, H, text, x, 14, scale, color);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x)
      if (x % 4 == 3 || y % 4 == 3) {
        uint8_t* p = &t.rgba[(size_t(y) * W + size_t(x)) * 4];
        p[0] = uint8_t(p[0] / 5); p[1] = uint8_t(p[1] / 5); p[2] = uint8_t(p[2] / 5);
      }
  return t;
}

glm::quat rotationOf(const glm::mat4& m) {
  return glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2]))));
}
glm::mat4 localMatrix(const ModelNode& n) {
  return glm::translate(glm::mat4(1.0f), n.translation) * glm::mat4_cast(n.rotation) * glm::scale(glm::mat4(1.0f), n.scale);
}
glm::mat4 globalOf(const std::vector<ModelNode>& pose, const glm::mat4& root, int i) {
  glm::mat4 g = localMatrix(pose[size_t(i)]);
  for (int p = pose[size_t(i)].parent; p >= 0; p = pose[size_t(p)].parent) g = localMatrix(pose[size_t(p)]) * g;
  return root * g;
}
glm::quat between(glm::vec3 a, glm::vec3 b) {
  a = glm::normalize(a); b = glm::normalize(b);
  const float c = glm::dot(a, b);
  if (c < -0.9999f) {
    glm::vec3 axis = glm::cross(glm::vec3(1, 0, 0), a);
    if (glm::length(axis) < 1e-3f) axis = glm::cross(glm::vec3(0, 1, 0), a);
    return glm::angleAxis(glm::pi<float>(), glm::normalize(axis));
  }
  const glm::vec3 axis = glm::cross(a, b);
  return glm::normalize(glm::quat(1.0f + c, axis.x, axis.y, axis.z));
}
// Gira o osso para que o segmento até o filho aponte na direção `dir` (mundo); o resto da cadeia acompanha.
void aimBone(std::vector<ModelNode>& pose, const glm::mat4& root, int bone, int child, glm::vec3 dir) {
  if (bone < 0 || child < 0 || glm::dot(dir, dir) < 1e-8f) return;
  const glm::mat4 gb = globalOf(pose, root, bone), gc = globalOf(pose, root, child);
  const glm::vec3 cur = glm::vec3(gc[3]) - glm::vec3(gb[3]);
  if (glm::dot(cur, cur) < 1e-12f) return;
  const glm::quat q = between(cur, dir);
  const int parent = pose[size_t(bone)].parent;
  const glm::quat parentRot = parent >= 0 ? rotationOf(globalOf(pose, root, parent)) : rotationOf(root);
  pose[size_t(bone)].rotation = glm::normalize(glm::inverse(parentRot) * (q * rotationOf(gb)));
}

// Nó do esqueleto Character Creator pelo nome curto ("R_Upperarm" -> "CC_Base_R_Upperarm_074").
int findCC(const Model& m, const std::string& shortName) {
  const std::string pre = "CC_Base_" + shortName + "_";
  for (size_t i = 0; i < m.nodeNames.size(); ++i) {
    const std::string& n = m.nodeNames[i];
    if (n.compare(0, pre.size(), pre) == 0 && n.find("scaleCompensation") == std::string::npos &&
        n.find_first_not_of("0123456789", pre.size()) == std::string::npos)
      return int(i);
  }
  return -1;
}

// Jogador realista: modelo escaneado com camisa e calção em materiais próprios e os clipes de captura de movimento.
int loadRealPlayer(Scene& s, Player& p) {
  int model = -1;
  const size_t matBase = s.materials.size();
  if (!loadGltf(s, core::gameDir() + "/jogador.glb", glm::mat4(1.0f), &model) || model < 0) return -1;
  Model& m = s.models[size_t(model)];
  p = Player{};
  p.model = model;
  p.real = true;
  // Altura de 1,82 m (o modelo tem 1,863 m na pose T) e pés no chão.
  p.fix = glm::scale(glm::translate(glm::mat4(1.0f), {0.0f, 0.0096f, 0.0f}), glm::vec3(1.82f / 1.863f));
  p.hips = findCC(m, "Hip"); p.spine = findCC(m, "Spine02"); p.head = findCC(m, "Head");
  const char* side[2] = {"R", "L"};
  for (int i = 0; i < 2; ++i) {
    const std::string S = side[i];
    p.arm[i] = findCC(m, S + "_Upperarm"); p.foreArm[i] = findCC(m, S + "_Forearm"); p.hand[i] = findCC(m, S + "_Hand");
    p.upLeg[i] = findCC(m, S + "_Thigh"); p.leg[i] = findCC(m, S + "_Calf"); p.foot[i] = findCC(m, S + "_Foot");
    p.toe[i] = findCC(m, S + "_ToeBase");
  }
  m.animation = m.findAnimation("k_idle");
  // Materiais próprios deste jogador; os do glTF entram em ordem a partir de matBase (10 = camisa, 11 = calção).
  auto own = [&](uint32_t obj) {
    const uint32_t orig = s.objects[obj].material;
    Material mat = s.materials[orig];
    const uint32_t idx = s.addMaterial(mat);
    s.objects[obj].material = idx;
    if (orig == matBase + 10) { s.materials[idx].roughness = 0.88f; s.materials[idx].metallic = 0.0f; p.shirtMats.push_back(idx); }
    if (orig == matBase + 11) { s.materials[idx].roughness = 0.8f; s.materials[idx].metallic = 0.0f; p.shortsMats.push_back(idx); }
  };
  for (uint32_t obj : m.skinnedObjects) own(obj);
  for (const auto& ro : m.rigidObjects) own(ro.second);
  if (p.shirtMats.empty()) LOG_WARN("Pênalti: o jogador não tem o material da camisa (T_shirt)");
  Player* pp = &p;
  m.poseHook = [pp](const Model& mm, std::vector<ModelNode>& pose) {
    const Player& P = *pp;
    if (P.anim.reachW <= 0.001f) return;
    // Braços do goleiro na bola perto da defesa, misturados com o clipe.
    for (int i = 0; i < 2; ++i) {
      for (int k = 0; k < 2; ++k) {
        const int bone = k == 0 ? P.arm[i] : P.foreArm[i], child = k == 0 ? P.foreArm[i] : P.hand[i];
        if (bone < 0 || child < 0) continue;
        const glm::quat before = pose[size_t(bone)].rotation;
        const glm::vec3 sh = glm::vec3(globalOf(pose, mm.root, P.arm[i])[3]);
        aimBone(pose, mm.root, bone, child, P.anim.reach - sh);
        pose[size_t(bone)].rotation = glm::normalize(glm::slerp(before, pose[size_t(bone)].rotation, P.anim.reachW));
      }
    }
  };
  return model;
}

int loadPlayer(Scene& s, Player& p) {
  int model = -1;
  if (!loadGltf(s, core::gameDir() + "/xbot.gltf", glm::mat4(1.0f), &model) || model < 0) return -1;
  Model& m = s.models[size_t(model)];
  p.model = model;
  auto node = [&](const char* name) { return m.findNode(std::string("mixamorig:") + name); };
  p.hips = node("Hips"); p.spine = node("Spine1"); p.head = node("Head");
  const char* side[2] = {"Right", "Left"};
  for (int i = 0; i < 2; ++i) {
    const std::string S = side[i];
    p.arm[i] = node((S + "Arm").c_str()); p.foreArm[i] = node((S + "ForeArm").c_str()); p.hand[i] = node((S + "Hand").c_str());
    p.upLeg[i] = node((S + "UpLeg").c_str()); p.leg[i] = node((S + "Leg").c_str()); p.foot[i] = node((S + "Foot").c_str());
    p.toe[i] = node((S + "ToeBase").c_str());
  }
  p.idle = m.findAnimation("idle"); p.run = m.findAnimation("run"); p.agree = m.findAnimation("agree");
  m.animation = p.idle;
  // Materiais próprios deste jogador (corpo = camisa, juntas = calção/meião), separados dos do outro.
  for (uint32_t obj : m.skinnedObjects) {
    Material mat = s.materials[s.objects[obj].material];
    mat.roughness = 0.62f;
    mat.metallic = 0.0f;
    const uint32_t idx = s.addMaterial(mat);
    s.objects[obj].material = idx;
  }
  if (m.skinnedObjects.size() >= 2) {
    p.jointMaterial = s.objects[m.skinnedObjects[0]].material;
    p.bodyMaterial = s.objects[m.skinnedObjects[1]].material;
  }
  const int pi = model;
  Player* pp = &p;
  m.poseHook = [pi, pp](const Model& mm, std::vector<ModelNode>& pose) {
    (void)pi;
    const Player& P = *pp;
    aimBone(pose, mm.root, P.spine, P.head, P.spineDir);
    for (int i = 0; i < 2; ++i) {
      aimBone(pose, mm.root, P.arm[i], P.foreArm[i], P.armDir[i]);
      aimBone(pose, mm.root, P.foreArm[i], P.hand[i], P.foreArmDir[i]);
      aimBone(pose, mm.root, P.upLeg[i], P.leg[i], P.upLegDir[i]);
      aimBone(pose, mm.root, P.leg[i], P.foot[i], P.legDir[i]);
      aimBone(pose, mm.root, P.foot[i], P.toe[i], P.footDir[i]);
    }
  };
  return model;
}

}  // namespace

void setKit(Scene& s, const Player& p, glm::vec3 shirt, glm::vec3 shorts) {
  auto set = [&](uint32_t m, glm::vec3 c) { s.materials[m].baseColor = glm::vec4(c, 1.0f); s.markMaterialDirty(m); };
  if (p.real) {
    for (uint32_t m : p.shirtMats) set(m, shirt);
    for (uint32_t m : p.shortsMats) set(m, shorts);
    return;
  }
  set(p.bodyMaterial, shirt);
  set(p.jointMaterial, shorts);
}

bool loadClipMetaInto(std::map<std::string, ClipMeta>& clips, float& fps);
bool loadClipMeta(PenaltiWorld& W) { return loadClipMetaInto(W.clips, W.clipFps) && W.clips.count("k_kick") && W.clips.count("gk_idle"); }

bool loadClipMetaInto(std::map<std::string, ClipMeta>& clipsOut, float& fps) {
  struct { std::map<std::string, ClipMeta>& clips; float& clipFps; } W{clipsOut, fps};
  std::ifstream f(core::gameDir() + "/jogador_anim.json");
  if (!f) return false;
  try {
    const nlohmann::json j = nlohmann::json::parse(f);
    W.clipFps = j.value("fps", 30.0f);
    for (const auto& [name, v] : j.at("meta").items()) {
      ClipMeta c;
      c.dur = v.value("dur", 0.0f);
      const auto& r = v.at("root");
      for (size_t i = 0; i + 1 < r.size(); i += 2) c.root.push_back({r[i].get<float>(), r[i + 1].get<float>()});
      if (v.contains("contact")) c.contact = v["contact"].get<float>();
      if (v.contains("foot")) { const auto& ft = v.at("foot"); c.foot = {ft[0].get<float>(), ft[1].get<float>(), ft[2].get<float>()}; }
      if (v.contains("reach")) { const auto& rc = v.at("reach"); c.reachT = rc.at("t").get<float>(); c.reach = {rc.at("x").get<float>(), rc.at("y").get<float>()}; }
      if (v.contains("speed")) c.speed = v.at("speed").get<float>();
      W.clips[name] = std::move(c);
    }
  } catch (const std::exception& e) {
    LOG_WARN("jogador_anim.json inválido (%s)", e.what());
    return false;
  }
  return !W.clips.empty();
}

bool buildPenaltiScene(Scene& s, Camera& camera, const DemoOptions& opt) {
  PenaltiWorld& W = world();
  W = PenaltiWorld{};
  const uint32_t cube = s.addMesh(makeCube()), cyl = s.addMesh(makeCylinder(32)), quad = s.addMesh(makeQuad());
  const uint32_t sphere = s.addMesh(makeSphere(64, 32));
  auto mat = [&](glm::vec3 albedo, float rough, float metal = 0.0f, uint32_t flags = 0) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    m.flags = flags;
    return s.addMaterial(m);
  };

  // ---- gramado: textura de grama com normal map, 2 m por repetição, e a marcação por cima (decal) ----
  {
    TextureData albedo, normal;
    Material g;
    g.baseColor = glm::vec4(0.62f, 0.86f, 0.5f, 1.0f);
    g.roughness = 0.88f;
    if (loadImageFile(core::gameDir() + "/grama.jpg", true, false, albedo)) g.baseColorTex = int32_t(s.addTexture(std::move(albedo)));
    if (loadImageFile(core::gameDir() + "/grama_nm.jpg", false, true, normal)) g.normalTex = int32_t(s.addTexture(std::move(normal)));
    g.uvScale = glm::vec2(60.0f);
    s.addObject(cube, trs({0.0f, -0.1f, 10.0f}, {120.0f, 0.1f, 120.0f}), s.addMaterial(g));
    const int32_t field = int32_t(s.addTexture(fieldTexture()));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(0.5f * (kFieldX1 - kFieldX0), 0.0f, 0.0f, 0.0f);
    m[1] = glm::vec4(0.0f, 0.0f, 0.5f * (kFieldZ1 - kFieldZ0), 0.0f);
    m[2] = glm::vec4(0.0f, 0.3f, 0.0f, 0.0f);
    m[3] = glm::vec4(0.5f * (kFieldX0 + kFieldX1), 0.0f, 0.5f * (kFieldZ0 + kFieldZ1), 1.0f);
    s.decals.push_back({glm::inverse(m), glm::vec4(1.0f), uint32_t(kDecalTexture), 0.0f, field, 0.8f});
    // tufos de grama 3D na área que a câmera vê de perto (balançam com o vento)
    Material grass;
    grass.baseColor = glm::vec4(0.09f, 0.2f, 0.05f, 1.0f);
    grass.roughness = 0.65f;
    grass.flags = kMaterialDoubleSided | kMaterialFoliage;
    const uint32_t grassMat = s.addMaterial(grass);
    uint32_t clumps[4];
    for (uint32_t v = 0; v < 4; ++v) clumps[v] = s.addMesh(makeGrassClump(16, 0.085f, v + 11));
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    for (int i = 0; i < 14000; ++i) {
      const float z = 0.15f + 13.6f * u(rng), x = (u(rng) * 2.0f - 1.0f) * (5.0f + 0.45f * z);
      if (std::abs(x) < 0.25f && std::abs(z - kSpotZ) < 0.25f) continue;  // a bola fica na marca
      glm::mat4 t = glm::translate(glm::mat4(1.0f), {x, -0.005f, z});
      t = glm::rotate(t, glm::two_pi<float>() * u(rng), glm::vec3(0, 1, 0));
      s.addObject(clumps[i % 4], glm::scale(t, glm::vec3(0.8f + 0.5f * u(rng))), grassMat);
    }
  }

  // ---- gol: traves de aço pintado, suportes e rede ----
  {
    const uint32_t paint = mat({0.92f, 0.92f, 0.9f}, 0.32f);
    const float r = 0.06f;
    for (int sx = -1; sx <= 1; sx += 2) {
      s.addObject(cyl, trs({float(sx) * kGoalHalfWidth, 0.5f * kGoalHeight, 0.0f}, {r, 0.5f * kGoalHeight, r}), paint);
      // suporte de trás (inclinado) e base no chão
      const glm::vec3 a(float(sx) * kGoalHalfWidth, kGoalHeight, 0.0f), b(float(sx) * kGoalHalfWidth, 0.0f, -kNetDepth);
      const float len = glm::length(b - a);
      glm::mat4 t = glm::translate(glm::mat4(1.0f), 0.5f * (a + b));
      t = glm::rotate(t, std::atan2(kNetDepth, kGoalHeight), glm::vec3(1, 0, 0));
      s.addObject(cyl, t * glm::scale(glm::mat4(1.0f), {0.025f, 0.5f * len, 0.025f}), paint);
      s.addObject(cyl, glm::rotate(trs({float(sx) * kGoalHalfWidth, 0.02f, -0.5f * kNetDepth}, glm::vec3(1.0f)), glm::half_pi<float>(), glm::vec3(1, 0, 0)) *
                           glm::scale(glm::mat4(1.0f), {0.025f, 0.5f * kNetDepth, 0.025f}), paint);
    }
    glm::mat4 bar = glm::translate(glm::mat4(1.0f), {0.0f, kGoalHeight, 0.0f});
    bar = glm::rotate(bar, glm::half_pi<float>(), glm::vec3(0, 0, 1));
    s.addObject(cyl, bar * glm::scale(glm::mat4(1.0f), {r, kGoalHalfWidth + r, r}), paint);
    glm::mat4 back = glm::translate(glm::mat4(1.0f), {0.0f, 0.02f, -kNetDepth});
    back = glm::rotate(back, glm::half_pi<float>(), glm::vec3(0, 0, 1));
    s.addObject(cyl, back * glm::scale(glm::mat4(1.0f), {0.025f, kGoalHalfWidth, 0.025f}), paint);

    Material net;
    net.baseColor = glm::vec4(0.95f, 0.95f, 0.93f, 1.0f);
    net.roughness = 0.9f;
    net.flags = kMaterialAlphaMask | kMaterialDoubleSided;
    net.alphaCutoff = 0.5f;
    net.baseColorTex = int32_t(s.addTexture(netTexture()));
    s.setTextureAlphaCutoff(net.baseColorTex, 0.5f);
    // fundo da rede: tecido simulado de 49 x 17 pontos (833 de no máximo 1.024), borda presa no quadro
    const uint32_t cw = 49, ch = 17;
    const float spacing = 2.0f * kGoalHalfWidth / float(cw - 1);
    MeshData cloth = makeClothGrid(cw, ch, spacing);
    cloth.clothPinned = false;
    cloth.clothPinBorder = true;
    W.netMesh = s.addMesh(std::move(cloth));
    Material backNet = net;
    backNet.uvScale = glm::vec2(float(cw - 1) * 0.5f, float(ch - 1) * 0.5f);
    W.netTransform = glm::translate(glm::mat4(1.0f), {-kGoalHalfWidth, float(ch - 1) * spacing, -kNetDepth});
    s.addObject(W.netMesh, W.netTransform, s.addMaterial(backNet));
    // teto e laterais da rede: planos parados com a mesma trama
    Material top = net;
    top.uvScale = glm::vec2(2.0f * kGoalHalfWidth / 0.3f, kNetDepth / 0.3f);
    s.addObject(quad, glm::rotate(trs({0.0f, kGoalHeight, -0.5f * kNetDepth}, glm::vec3(1.0f)), -glm::half_pi<float>(), glm::vec3(1, 0, 0)) *
                          glm::scale(glm::mat4(1.0f), {kGoalHalfWidth, 0.5f * kNetDepth, 1.0f}), s.addMaterial(top));
    Material side = net;
    side.uvScale = glm::vec2(kNetDepth / 0.3f, kGoalHeight / 0.3f);
    const uint32_t sideMat = s.addMaterial(side);
    for (int sx = -1; sx <= 1; sx += 2)
      s.addObject(quad, glm::rotate(trs({float(sx) * kGoalHalfWidth, 0.5f * kGoalHeight, -0.5f * kNetDepth}, glm::vec3(1.0f)), glm::half_pi<float>(), glm::vec3(0, 1, 0)) *
                            glm::scale(glm::mat4(1.0f), {0.5f * kNetDepth, 0.5f * kGoalHeight, 1.0f}), sideMat);
  }

  // ---- placas de LED (luz de malha: iluminam o gramado de verdade) ----
  {
    Material led;
    led.baseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
    led.roughness = 0.35f;
    W.ledTexture = int32_t(s.addTexture(ledTexture("PENALTI DA TRETA  *  BRASILEIRAO 2026  *  ", {255, 176, 32, 255})));
    led.emissiveTex = W.ledTexture;
    led.emissive = glm::vec3(900.0f);  // nits
    led.uvScale = glm::vec2(2.0f, 1.0f);
    W.ledMaterial = s.addMaterial(led);
    s.addObject(quad, trs({0.0f, 0.5f, -4.8f}, {22.0f, 0.48f, 1.0f}), W.ledMaterial);
    const uint32_t frame = mat({0.03f, 0.03f, 0.035f}, 0.6f, 0.3f);
    s.addObject(cube, trs({0.0f, 0.5f, -4.86f}, {22.2f, 0.52f, 0.05f}), frame);
    for (int sx = -1; sx <= 1; sx += 2) {
      s.addObject(quad, trs({float(sx) * 23.0f, 0.5f, 9.0f}, {14.0f, 0.48f, 1.0f}, -float(sx) * glm::half_pi<float>()), W.ledMaterial);
      s.addObject(cube, trs({float(sx) * 23.06f, 0.5f, 9.0f}, {0.05f, 0.52f, 14.2f}), frame);
    }
  }

  // ---- arquibancadas, cobertura e torcida ----
  {
    const uint32_t concrete = mat({0.32f, 0.32f, 0.34f}, 0.92f), roof = mat({0.08f, 0.085f, 0.1f}, 0.5f, 0.6f);
    const uint32_t seatA = mat({0.05f, 0.18f, 0.5f}, 0.45f), seatB = mat({0.55f, 0.06f, 0.07f}, 0.45f);
    // torcedores: o mesmo personagem assado (16 poses do clipe "parado"), com camisas de cores diferentes
    Player crowdSrc;
    const int cm = loadPlayer(s, crowdSrc);
    if (cm >= 0) {
      Model& m = s.models[size_t(cm)];
      m.poseHook = nullptr;
      m.animation = crowdSrc.agree >= 0 ? crowdSrc.agree : crowdSrc.idle;
      m.root = glm::translate(glm::mat4(1.0f), {0.0f, -100.0f, 0.0f});
      const uint32_t clip = s.bakeCrowdClip(cm, 16, 1);
      const glm::vec3 shirts[6] = {{0.6f, 0.04f, 0.05f}, {0.02f, 0.02f, 0.02f}, {0.85f, 0.85f, 0.82f}, {0.03f, 0.25f, 0.08f}, {0.75f, 0.6f, 0.02f}, {0.02f, 0.08f, 0.4f}};
      for (const glm::vec3& c : shirts) {
        CrowdClip cc = s.crowdClips[clip];
        cc.material = mat(c, 0.75f);
        s.crowdClips.push_back(cc);
        W.crowdClips.push_back(uint32_t(s.crowdClips.size() - 1));
      }
    }
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    struct Stand { glm::vec3 c; float len, yaw; };
    const Stand stands[3] = {{{0.0f, 0.0f, -9.0f}, 76.0f, 0.0f}, {{-31.0f, 0.0f, 14.0f}, 52.0f, glm::half_pi<float>()}, {{31.0f, 0.0f, 14.0f}, 52.0f, -glm::half_pi<float>()}};
    const int rows = 14;
    const float base = 2.2f, rise = 0.62f, depth = 1.15f;
    for (const Stand& st : stands) {
      const glm::mat4 S = glm::rotate(glm::translate(glm::mat4(1.0f), st.c), st.yaw, glm::vec3(0, 1, 0));
      s.addObject(cube, S * trs({0.0f, 0.5f * base, 0.0f}, {0.5f * st.len, 0.5f * base, 0.2f}), concrete);
      for (int r = 0; r < rows; ++r) {
        const float y = base + float(r) * rise, z = -0.8f - float(r) * depth;
        s.addObject(cube, S * trs({0.0f, 0.5f * y, z}, {0.5f * st.len, 0.5f * y, 0.5f * depth}), concrete);
        s.addObject(cube, S * trs({0.0f, y + 0.22f, z + 0.15f}, {0.5f * st.len, 0.04f, 0.2f}), (r / 3) % 2 ? seatA : seatB);
        if (W.crowdClips.empty()) continue;
        for (float x = -0.5f * st.len + 0.5f; x < 0.5f * st.len - 0.5f; x += 0.62f) {
          if (u(rng) < 0.1f) continue;
          const glm::vec3 p(x + (u(rng) - 0.5f) * 0.12f, y + 0.05f, z + (u(rng) - 0.5f) * 0.1f);
          glm::mat4 t = S * glm::translate(glm::mat4(1.0f), p);
          t = glm::rotate(t, (u(rng) - 0.5f) * 0.5f, glm::vec3(0, 1, 0));
          t = glm::scale(t, glm::vec3(0.9f + 0.16f * u(rng)));
          const uint32_t clip = W.crowdClips[size_t(u(rng) * float(W.crowdClips.size())) % W.crowdClips.size()];
          s.addCrowdAgent(clip, t, 4.0f * u(rng));
        }
      }
      // cobertura com uma faixa de luz por baixo da borda
      s.addObject(cube, S * trs({0.0f, base + rows * rise + 5.6f, -0.5f * rows * depth - 1.0f}, {0.5f * st.len, 0.2f, 0.5f * rows * depth + 1.5f}), roof);
      Material strip;
      strip.baseColor = glm::vec4(0.1f);
      strip.emissive = glm::vec3(5200.0f, 4800.0f, 4100.0f);
      s.addObject(cube, S * trs({0.0f, base + rows * rise + 5.3f, 0.4f}, {0.5f * st.len, 0.05f, 0.05f}), s.addMaterial(strip));
    }
  }

  // ---- torres de refletores: painéis emissivos (luz de malha) apontados para a área ----
  {
    const uint32_t steel = mat({0.12f, 0.13f, 0.15f}, 0.45f, 0.8f);
    Material lampM;
    lampM.baseColor = glm::vec4(0.05f);
    lampM.emissiveTex = int32_t(s.addTexture(lampTexture()));
    lampM.emissive = glm::vec3(320000.0f);  // nits: ~1.500 lux na grande área a ~50 m
    const uint32_t lampMat = s.addMaterial(lampM);
    const glm::vec2 towers[4] = {{-27.0f, -16.0f}, {27.0f, -16.0f}, {-36.0f, 34.0f}, {36.0f, 34.0f}};
    for (const glm::vec2& t : towers) {
      const float h = 34.0f;
      s.addObject(cyl, trs({t.x, 0.5f * h, t.y}, {0.45f, 0.5f * h, 0.45f}), steel);
      const glm::vec3 head(t.x, h + 1.8f, t.y), target(0.0f, 0.0f, 6.0f);
      const glm::vec3 fwd = glm::normalize(target - head);
      const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd)), up = glm::cross(fwd, right);
      glm::mat4 o(1.0f);
      o[0] = glm::vec4(right, 0.0f); o[1] = glm::vec4(up, 0.0f); o[2] = glm::vec4(fwd, 0.0f); o[3] = glm::vec4(head, 1.0f);
      s.addObject(cube, o * glm::scale(glm::mat4(1.0f), {4.3f, 2.3f, 0.25f}), steel);
      s.addObject(quad, o * glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.27f}) * glm::scale(glm::mat4(1.0f), {4.0f, 2.0f, 1.0f}), lampMat);
    }
  }

  // ---- bola ----
  {
    Material b;
    b.baseColor = glm::vec4(1.0f);
    b.roughness = 0.42f;
    b.clearcoat = 0.6f;
    b.clearcoatRoughness = 0.25f;
    b.baseColorTex = int32_t(s.addTexture(ballTexture()));
    W.ball = s.addObject(sphere, trs({0.0f, kBallRadius, kSpotZ}, glm::vec3(kBallRadius)), s.addMaterial(b));
  }

  // ---- jogadores ----
  // Realistas (escaneados, com captura de movimento) quando os arquivos existem; senão, o manequim.
  if (loadClipMeta(W) && loadRealPlayer(s, W.kicker) >= 0 && loadRealPlayer(s, W.keeper) >= 0) {
    LOG_INFO("Pênalti: jogadores realistas com %zu clipes de captura de movimento", W.clips.size());
  } else if (loadPlayer(s, W.kicker) < 0 || loadPlayer(s, W.keeper) < 0) {
    LOG_ERROR("Pênalti: não achei o personagem em %s/xbot.gltf", core::gameDir().c_str());
    return false;
  }
  setKit(s, W.kicker, {0.62f, 0.03f, 0.05f}, {0.02f, 0.02f, 0.02f});
  setKit(s, W.keeper, {0.1f, 0.6f, 0.18f}, {0.02f, 0.02f, 0.02f});
  s.models[size_t(W.kicker.model)].root = glm::rotate(glm::translate(glm::mat4(1.0f), {-1.55f, 0.0f, 11.95f}), glm::pi<float>() - 0.45f, glm::vec3(0, 1, 0)) * W.kicker.fix;
  s.models[size_t(W.keeper.model)].root = glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.35f}) * W.keeper.fix;
  // goleiro esperando: braços abertos à frente, joelhos dobrados
  if (!W.keeper.real) {
    Player& k = W.keeper;
    for (int i = 0; i < 2; ++i) {
      const float sx = i == 0 ? -1.0f : 1.0f;  // direita do goleiro (que olha para +z) fica em -x
      k.armDir[i] = {0.75f * sx, -0.35f, 0.55f};
      k.foreArmDir[i] = {0.45f * sx, 0.1f, 0.9f};
      k.upLegDir[i] = {0.28f * sx, -1.0f, 0.3f};
      k.legDir[i] = {0.1f * sx, -1.0f, -0.22f};
    }
  }

  // ---- noite: céu, lua, névoa leve (raios de luz dos refletores) ----
  s.sunIlluminance = 0.0f;
  s.moonIlluminance = 0.25f;
  s.moonDirection = glm::normalize(glm::vec3(-0.4f, 0.5f, -0.6f));
  s.fogDensity = opt.fog >= 0.0f ? opt.fog : 0.004f;
  s.fogFalloff = 0.05f;
  camera.position = glm::vec3(-2.6f, 1.35f, 15.4f);
  camera.lookAt(glm::vec3(0.0f, 1.1f, 0.0f));
  camera.fovY = glm::radians(42.0f);
  LOG_INFO("Pênalti: estádio com %zu torcedores", size_t(std::count_if(s.objects.begin(), s.objects.end(), [](const Object& o) { return o.crowdClip != ~0u; })));
  return true;
}


// =====================================================================================================================
// FCLegacy: estádio da partida 11 x 11. Campo de 105 x 68 m com o centro na origem, gols em x = ±52,5 (eixos iguais
// aos da simulação fcsim). Mesmos materiais e luzes do estádio do pênalti, agora em volta do campo inteiro.
// =====================================================================================================================

namespace {

constexpr float kPL = 52.5f, kPW = 34.0f;  // meio comprimento e meia largura
constexpr float kMX0 = -56.0f, kMX1 = 56.0f, kMZ0 = -37.0f, kMZ1 = 37.0f;  // área da textura de marcação

TextureData fullFieldTexture() {
  const int W = 4096, H = 2720;
  TextureData t = blankTexture(W, H, {0, 0, 0, 0});
  t.name = "marcacao_campo";
  auto px = [&](float x, float z) { return glm::vec2((x - kMX0) / (kMX1 - kMX0) * W, (kMZ1 - z) / (kMZ1 - kMZ0) * H); };
  auto put = [&](int x, int y, glm::u8vec4 c, float a) {
    if (x < 0 || y < 0 || x >= W || y >= H) return;
    uint8_t* p = &t.rgba[(size_t(y) * W + size_t(x)) * 4];
    for (int i = 0; i < 3; ++i) p[i] = uint8_t(float(p[i]) * (1.0f - a) + float(c[i]) * a);
    p[3] = uint8_t(std::max(float(p[3]), 255.0f * a));
  };
  // faixas do corte da grama a cada 5,25 m no comprimento (20 faixas no campo)
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      const float wx = kMX0 + (float(x) + 0.5f) / W * (kMX1 - kMX0);
      const bool dark = int(std::floor((wx + kPL) / 5.25f)) % 2 == 0;
      uint8_t* p = &t.rgba[(size_t(y) * W + size_t(x)) * 4];
      const glm::u8vec4 c = dark ? glm::u8vec4(18, 46, 14, 255) : glm::u8vec4(58, 96, 30, 255);
      p[0] = c.r; p[1] = c.g; p[2] = c.b; p[3] = 38;
    }
  const float lw = 0.12f;
  auto line = [&](glm::vec2 a, glm::vec2 b) {
    const glm::vec2 pa = px(a.x, a.y), pb = px(b.x, b.y);
    const int steps = int(glm::length(pb - pa) * 2.0f) + 2;
    const float rx = lw * 0.5f / ((kMX1 - kMX0) / W), rz = lw * 0.5f / ((kMZ1 - kMZ0) / H);
    for (int i = 0; i <= steps; ++i) {
      const glm::vec2 c = glm::mix(pa, pb, float(i) / float(steps));
      for (int dy = -int(rz + 1); dy <= int(rz + 1); ++dy)
        for (int dx = -int(rx + 1); dx <= int(rx + 1); ++dx) {
          const float k = std::max(std::abs(float(dx)) / std::max(rx, 0.5f), std::abs(float(dy)) / std::max(rz, 0.5f));
          if (k <= 1.0f) put(int(c.x) + dx, int(c.y) + dy, {236, 238, 232, 255}, 0.92f);
        }
    }
  };
  auto arc = [&](glm::vec2 c, float r, float a0, float a1, int n) {
    for (int i = 0; i < n; ++i) {
      const float u0 = a0 + (a1 - a0) * float(i) / float(n), u1 = a0 + (a1 - a0) * float(i + 1) / float(n);
      line(c + r * glm::vec2(std::cos(u0), std::sin(u0)), c + r * glm::vec2(std::cos(u1), std::sin(u1)));
    }
  };
  line({-kPL, -kPW}, {kPL, -kPW}); line({-kPL, kPW}, {kPL, kPW});        // laterais
  line({-kPL, -kPW}, {-kPL, kPW}); line({kPL, -kPW}, {kPL, kPW});        // linhas de fundo
  line({0.0f, -kPW}, {0.0f, kPW});                                       // meio-campo
  arc({0.0f, 0.0f}, 9.15f, 0.0f, glm::two_pi<float>(), 96);              // círculo central
  arc({0.0f, 0.0f}, 0.12f, 0.0f, glm::two_pi<float>(), 12);
  for (int s = -1; s <= 1; s += 2) {
    const float gx = float(s) * kPL, in = -float(s);
    line({gx, -20.16f}, {gx + in * 16.5f, -20.16f}); line({gx, 20.16f}, {gx + in * 16.5f, 20.16f});
    line({gx + in * 16.5f, -20.16f}, {gx + in * 16.5f, 20.16f});           // grande área
    line({gx, -9.16f}, {gx + in * 5.5f, -9.16f}); line({gx, 9.16f}, {gx + in * 5.5f, 9.16f});
    line({gx + in * 5.5f, -9.16f}, {gx + in * 5.5f, 9.16f});               // pequena área
    const glm::vec2 spot(gx + in * 11.0f, 0.0f);
    arc(spot, 0.12f, 0.0f, glm::two_pi<float>(), 12);                     // marca do pênalti
    const float half = std::acos(5.5f / 9.15f);                           // meia-lua fora da grande área
    arc(spot, 9.15f, (s > 0 ? glm::pi<float>() : 0.0f) - half, (s > 0 ? glm::pi<float>() : 0.0f) + half, 32);
    for (int c = -1; c <= 1; c += 2) {                                    // escanteios (raio de 1 m)
      const float base = std::atan2(-float(c), in);
      arc({gx, float(c) * kPW}, 1.0f, base - glm::quarter_pi<float>(), base + glm::quarter_pi<float>(), 10);
    }
  }
  return t;
}

// Gol com rede de tecido, no espaço local: boca ao longo de x, rede para -z. G leva para o mundo.
void buildGoal(Scene& s, const glm::mat4& G, uint32_t cyl, uint32_t quad, uint32_t paint, uint32_t& netMesh, glm::mat4& netTransform) {
  const float r = 0.06f;
  for (int sx = -1; sx <= 1; sx += 2) {
    s.addObject(cyl, G * trs({float(sx) * kGoalHalfWidth, 0.5f * kGoalHeight, 0.0f}, {r, 0.5f * kGoalHeight, r}), paint);
    const glm::vec3 a(float(sx) * kGoalHalfWidth, kGoalHeight, 0.0f), b(float(sx) * kGoalHalfWidth, 0.0f, -kNetDepth);
    glm::mat4 t = glm::translate(glm::mat4(1.0f), 0.5f * (a + b));
    t = glm::rotate(t, std::atan2(kNetDepth, kGoalHeight), glm::vec3(1, 0, 0));
    s.addObject(cyl, G * t * glm::scale(glm::mat4(1.0f), {0.025f, 0.5f * glm::length(b - a), 0.025f}), paint);
  }
  glm::mat4 bar = glm::rotate(glm::translate(glm::mat4(1.0f), {0.0f, kGoalHeight, 0.0f}), glm::half_pi<float>(), glm::vec3(0, 0, 1));
  s.addObject(cyl, G * bar * glm::scale(glm::mat4(1.0f), {r, kGoalHalfWidth + r, r}), paint);
  Material net;
  net.baseColor = glm::vec4(0.95f, 0.95f, 0.93f, 1.0f);
  net.roughness = 0.9f;
  net.flags = kMaterialAlphaMask | kMaterialDoubleSided;
  net.alphaCutoff = 0.5f;
  net.baseColorTex = int32_t(s.addTexture(netTexture()));
  s.setTextureAlphaCutoff(net.baseColorTex, 0.5f);
  const uint32_t cw = 49, ch = 17;
  const float spacing = 2.0f * kGoalHalfWidth / float(cw - 1);
  MeshData cloth = makeClothGrid(cw, ch, spacing);
  cloth.clothPinned = false;
  cloth.clothPinBorder = true;
  netMesh = s.addMesh(std::move(cloth));
  Material backNet = net;
  backNet.uvScale = glm::vec2(float(cw - 1) * 0.5f, float(ch - 1) * 0.5f);
  netTransform = G * glm::translate(glm::mat4(1.0f), {-kGoalHalfWidth, float(ch - 1) * spacing, -kNetDepth});
  s.addObject(netMesh, netTransform, s.addMaterial(backNet));
  Material top = net;
  top.uvScale = glm::vec2(2.0f * kGoalHalfWidth / 0.3f, kNetDepth / 0.3f);
  s.addObject(quad, G * glm::rotate(trs({0.0f, kGoalHeight, -0.5f * kNetDepth}, glm::vec3(1.0f)), -glm::half_pi<float>(), glm::vec3(1, 0, 0)) *
                        glm::scale(glm::mat4(1.0f), {kGoalHalfWidth, 0.5f * kNetDepth, 1.0f}), s.addMaterial(top));
  Material side = net;
  side.uvScale = glm::vec2(kNetDepth / 0.3f, kGoalHeight / 0.3f);
  const uint32_t sideMat = s.addMaterial(side);
  for (int sx = -1; sx <= 1; sx += 2)
    s.addObject(quad, G * glm::rotate(trs({float(sx) * kGoalHalfWidth, 0.5f * kGoalHeight, -0.5f * kNetDepth}, glm::vec3(1.0f)), glm::half_pi<float>(), glm::vec3(0, 1, 0)) *
                          glm::scale(glm::mat4(1.0f), {0.5f * kNetDepth, 0.5f * kGoalHeight, 1.0f}), sideMat);
}

}  // namespace

PartidaWorld& partidaWorld() {
  static PartidaWorld w;
  return w;
}

bool buildPartidaScene(Scene& s, Camera& camera, const DemoOptions& opt) {
  PartidaWorld& W = partidaWorld();
  W = PartidaWorld{};
  const uint32_t cube = s.addMesh(makeCube()), cyl = s.addMesh(makeCylinder(32)), quad = s.addMesh(makeQuad());
  const uint32_t sphere = s.addMesh(makeSphere(64, 32));
  auto mat = [&](glm::vec3 albedo, float rough, float metal = 0.0f) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    return s.addMaterial(m);
  };

  // ---- gramado e marcação ----
  {
    TextureData albedo, normal;
    Material g;
    g.baseColor = glm::vec4(0.62f, 0.86f, 0.5f, 1.0f);
    g.roughness = 0.88f;
    if (loadImageFile(core::gameDir() + "/grama.jpg", true, false, albedo)) g.baseColorTex = int32_t(s.addTexture(std::move(albedo)));
    if (loadImageFile(core::gameDir() + "/grama_nm.jpg", false, true, normal)) g.normalTex = int32_t(s.addTexture(std::move(normal)));
    g.uvScale = glm::vec2(80.0f);
    s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {80.0f, 0.1f, 60.0f}), s.addMaterial(g));
    const int32_t field = int32_t(s.addTexture(fullFieldTexture()));
    glm::mat4 m(1.0f);
    m[0] = glm::vec4(0.5f * (kMX1 - kMX0), 0.0f, 0.0f, 0.0f);
    m[1] = glm::vec4(0.0f, 0.0f, 0.5f * (kMZ1 - kMZ0), 0.0f);
    m[2] = glm::vec4(0.0f, 0.3f, 0.0f, 0.0f);
    m[3] = glm::vec4(0.5f * (kMX0 + kMX1), 0.0f, 0.5f * (kMZ0 + kMZ1), 1.0f);
    s.decals.push_back({glm::inverse(m), glm::vec4(1.0f), uint32_t(kDecalTexture), 0.0f, field, 0.8f});
  }

  // ---- gols: a boca olha para o centro; a rede fica atrás da linha ----
  {
    const uint32_t paint = mat({0.92f, 0.92f, 0.9f}, 0.32f);
    for (int k = 0; k < 2; ++k) {
      const float side = k == 0 ? -1.0f : 1.0f;  // gol em x = side * 52,5
      // local -z (rede) -> mundo +side*x
      const glm::mat4 G = glm::rotate(glm::translate(glm::mat4(1.0f), {side * kPL, 0.0f, 0.0f}), side * -glm::half_pi<float>(), glm::vec3(0, 1, 0));
      buildGoal(s, G, cyl, quad, paint, W.netMesh[k], W.netTransform[k]);
    }
  }

  // ---- placas de LED em volta do campo ----
  {
    Material led;
    led.baseColor = glm::vec4(0.02f, 0.02f, 0.02f, 1.0f);
    led.roughness = 0.35f;
    W.ledTexture = int32_t(s.addTexture(ledTexture("FCLEGACY  *  BRASILEIRAO 2026  *  LEGACY ENGINE  *  ", {255, 176, 32, 255})));
    led.emissiveTex = W.ledTexture;
    led.emissive = glm::vec3(900.0f);
    led.uvScale = glm::vec2(6.0f, 1.0f);
    W.ledMaterial = s.addMaterial(led);
    const uint32_t frame = mat({0.03f, 0.03f, 0.035f}, 0.6f, 0.3f);
    for (int sz = -1; sz <= 1; sz += 2) {  // laterais (a câmera fica em +z: a placa de -z aparece ao fundo)
      s.addObject(quad, trs({0.0f, 0.5f, float(sz) * 38.0f}, {50.0f, 0.48f, 1.0f}, sz > 0 ? glm::pi<float>() : 0.0f), W.ledMaterial);
      s.addObject(cube, trs({0.0f, 0.5f, float(sz) * 38.06f}, {50.2f, 0.52f, 0.05f}), frame);
    }
    for (int sx = -1; sx <= 1; sx += 2) {  // atrás dos gols
      s.addObject(quad, trs({float(sx) * 57.0f, 0.5f, 0.0f}, {30.0f, 0.48f, 1.0f}, -float(sx) * glm::half_pi<float>()), W.ledMaterial);
      s.addObject(cube, trs({float(sx) * 57.06f, 0.5f, 0.0f}, {0.05f, 0.52f, 30.2f}), frame);
    }
  }

  // ---- arquibancadas, cobertura e torcida (o manequim assado em 6 cores de camisa) ----
  {
    const uint32_t concrete = mat({0.32f, 0.32f, 0.34f}, 0.92f), roof = mat({0.08f, 0.085f, 0.1f}, 0.5f, 0.6f);
    const uint32_t seatA = mat({0.05f, 0.18f, 0.5f}, 0.45f), seatB = mat({0.55f, 0.06f, 0.07f}, 0.45f);
    std::vector<uint32_t> crowdClips;
    Player crowdSrc;
    const int cm = loadPlayer(s, crowdSrc);
    if (cm >= 0) {
      Model& m = s.models[size_t(cm)];
      m.poseHook = nullptr;
      m.animation = crowdSrc.agree >= 0 ? crowdSrc.agree : crowdSrc.idle;
      m.root = glm::translate(glm::mat4(1.0f), {0.0f, -100.0f, 0.0f});
      const uint32_t clip = s.bakeCrowdClip(cm, 16, 1);
      const glm::vec3 shirts[6] = {{0.6f, 0.04f, 0.05f}, {0.02f, 0.02f, 0.02f}, {0.85f, 0.85f, 0.82f}, {0.03f, 0.25f, 0.08f}, {0.75f, 0.6f, 0.02f}, {0.02f, 0.08f, 0.4f}};
      for (const glm::vec3& c : shirts) {
        CrowdClip cc = s.crowdClips[clip];
        cc.material = mat(c, 0.75f);
        s.crowdClips.push_back(cc);
        crowdClips.push_back(uint32_t(s.crowdClips.size() - 1));
      }
    }
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    struct Stand { glm::vec3 c; float len, yaw; };
    const Stand stands[4] = {{{0.0f, 0.0f, -41.0f}, 124.0f, 0.0f}, {{0.0f, 0.0f, 41.0f}, 124.0f, glm::pi<float>()},
                             {{-60.0f, 0.0f, 0.0f}, 80.0f, glm::half_pi<float>()}, {{60.0f, 0.0f, 0.0f}, 80.0f, -glm::half_pi<float>()}};
    const int rows = 12;
    const float base = 2.2f, rise = 0.62f, depth = 1.15f;
    for (const Stand& st : stands) {
      const glm::mat4 S = glm::rotate(glm::translate(glm::mat4(1.0f), st.c), st.yaw, glm::vec3(0, 1, 0));
      s.addObject(cube, S * trs({0.0f, 0.5f * base, 0.0f}, {0.5f * st.len, 0.5f * base, 0.2f}), concrete);
      for (int r = 0; r < rows; ++r) {
        const float y = base + float(r) * rise, z = -0.8f - float(r) * depth;
        s.addObject(cube, S * trs({0.0f, 0.5f * y, z}, {0.5f * st.len, 0.5f * y, 0.5f * depth}), concrete);
        s.addObject(cube, S * trs({0.0f, y + 0.22f, z + 0.15f}, {0.5f * st.len, 0.04f, 0.2f}), (r / 3) % 2 ? seatA : seatB);
        if (crowdClips.empty()) continue;
        for (float x = -0.5f * st.len + 0.5f; x < 0.5f * st.len - 0.5f; x += 0.7f) {
          if (u(rng) < 0.12f) continue;
          const glm::vec3 p(x + (u(rng) - 0.5f) * 0.12f, y + 0.05f, z + (u(rng) - 0.5f) * 0.1f);
          glm::mat4 t = S * glm::translate(glm::mat4(1.0f), p);
          t = glm::rotate(t, (u(rng) - 0.5f) * 0.5f, glm::vec3(0, 1, 0));
          t = glm::scale(t, glm::vec3(0.9f + 0.16f * u(rng)));
          s.addCrowdAgent(crowdClips[size_t(u(rng) * float(crowdClips.size())) % crowdClips.size()], t, 4.0f * u(rng));
        }
      }
      s.addObject(cube, S * trs({0.0f, base + rows * rise + 5.6f, -0.5f * rows * depth - 1.0f}, {0.5f * st.len, 0.2f, 0.5f * rows * depth + 1.5f}), roof);
      Material strip;
      strip.baseColor = glm::vec4(0.1f);
      strip.emissive = glm::vec3(5200.0f, 4800.0f, 4100.0f);
      s.addObject(cube, S * trs({0.0f, base + rows * rise + 5.3f, 0.4f}, {0.5f * st.len, 0.05f, 0.05f}), s.addMaterial(strip));
    }
  }

  // ---- torres de refletores nos quatro cantos ----
  {
    const uint32_t steel = mat({0.12f, 0.13f, 0.15f}, 0.45f, 0.8f);
    Material lampM;
    lampM.baseColor = glm::vec4(0.05f);
    lampM.emissiveTex = int32_t(s.addTexture(lampTexture()));
    lampM.emissive = glm::vec3(420000.0f);
    const uint32_t lampMat = s.addMaterial(lampM);
    const glm::vec2 towers[4] = {{-66.0f, -50.0f}, {66.0f, -50.0f}, {-66.0f, 50.0f}, {66.0f, 50.0f}};
    for (const glm::vec2& t : towers) {
      const float h = 40.0f;
      s.addObject(cyl, trs({t.x, 0.5f * h, t.y}, {0.5f, 0.5f * h, 0.5f}), steel);
      const glm::vec3 head(t.x, h + 1.8f, t.y), target(t.x * 0.25f, 0.0f, t.y * 0.2f);
      const glm::vec3 fwd = glm::normalize(target - head);
      const glm::vec3 right = glm::normalize(glm::cross(glm::vec3(0, 1, 0), fwd)), up = glm::cross(fwd, right);
      glm::mat4 o(1.0f);
      o[0] = glm::vec4(right, 0.0f); o[1] = glm::vec4(up, 0.0f); o[2] = glm::vec4(fwd, 0.0f); o[3] = glm::vec4(head, 1.0f);
      s.addObject(cube, o * glm::scale(glm::mat4(1.0f), {5.3f, 2.8f, 0.25f}), steel);
      s.addObject(quad, o * glm::translate(glm::mat4(1.0f), {0.0f, 0.0f, 0.27f}) * glm::scale(glm::mat4(1.0f), {5.0f, 2.5f, 1.0f}), lampMat);
    }
  }

  // ---- bola ----
  {
    Material b;
    b.baseColor = glm::vec4(1.0f);
    b.roughness = 0.42f;
    b.clearcoat = 0.6f;
    b.clearcoatRoughness = 0.25f;
    b.baseColorTex = int32_t(s.addTexture(ballTexture()));
    W.ball = s.addObject(sphere, trs({0.0f, kBallRadius, 0.0f}, glm::vec3(kBallRadius)), s.addMaterial(b));
  }

  // ---- 22 jogadores realistas (o mesmo modelo, texturas compartilhadas) ----
  if (!loadClipMetaInto(W.clips, W.clipFps)) {
    LOG_ERROR("FCLegacy: não achei game/jogador_anim.json");
    return false;
  }
  for (int i = 0; i < 22; ++i) {
    if (loadRealPlayer(s, W.players[size_t(i)]) < 0) {
      LOG_ERROR("FCLegacy: não achei game/jogador.glb");
      return false;
    }
    s.models[size_t(W.players[size_t(i)].model)].root = glm::translate(glm::mat4(1.0f), {0.0f, -200.0f, 0.0f});
  }

  // ---- noite ----
  s.sunIlluminance = 0.0f;
  s.moonIlluminance = 0.25f;
  s.moonDirection = glm::normalize(glm::vec3(-0.4f, 0.5f, -0.6f));
  s.fogDensity = opt.fog >= 0.0f ? opt.fog : 0.003f;
  s.fogFalloff = 0.05f;
  camera.position = glm::vec3(0.0f, 20.0f, 62.0f);
  camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f));
  camera.fovY = glm::radians(30.0f);
  LOG_INFO("FCLegacy: estádio com %zu torcedores e 22 jogadores", size_t(std::count_if(s.objects.begin(), s.objects.end(), [](const Object& o) { return o.crowdClip != ~0u; })));
  return true;
}

}  // namespace game
