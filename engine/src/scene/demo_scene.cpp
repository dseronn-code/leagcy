#include "scene/demo_scene.h"

#include <cmath>
#include <random>

#include <glm/gtc/matrix_transform.hpp>

namespace scene {

namespace {

glm::mat4 trs(glm::vec3 t, glm::vec3 s, float yawDeg = 0.0f) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
  m = glm::rotate(m, glm::radians(yawDeg), glm::vec3(0, 1, 0));
  return glm::scale(m, s);
}

glm::vec3 sunFrom(float elevationDeg, float azimuthDeg) {
  float e = glm::radians(elevationDeg), a = glm::radians(azimuthDeg);
  return glm::normalize(glm::vec3(std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)));
}

// Cor saturada a partir de um matiz (0..1).
glm::vec3 hue(float h) {
  glm::vec3 c = glm::clamp(glm::abs(glm::mod(h * 6.0f + glm::vec3(0, 4, 2), 6.0f) - 3.0f) - 1.0f, 0.0f, 1.0f);
  return c * c;  // aproximadamente linear
}

float terrainHeight(float x, float z) {
  float dist = std::sqrt(x * x + z * z);
  float hills = (fbm(x * 0.006f + 3.1f, z * 0.006f + 7.7f, 6) - 0.45f) * 90.0f;
  float mask = glm::smoothstep(35.0f, 160.0f, dist);  // área central plana para a vitrine
  float bumps = (fbm(x * 0.12f, z * 0.12f, 3) - 0.5f) * 0.35f;
  return hills * mask + bumps;
}

const std::string kAssets = ASSET_DIR;

bool buildOutdoor(Scene& s, Camera& camera, const DemoOptions& opt) {
  uint32_t terrain = s.addMesh(makeTerrain(1200.0f, 640, terrainHeight));
  uint32_t sphere = s.addMesh(makeSphere());
  uint32_t cube = s.addMesh(makeCube());

  Material ground;
  ground.flags = kMaterialTerrain;
  s.addObject(terrain, glm::mat4(1.0f), s.addMaterial(ground));

  auto mat = [&](glm::vec3 albedo, float rough, float metal, glm::vec3 emissive = glm::vec3(0.0f)) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    m.emissive = emissive;
    return s.addMaterial(m);
  };

  for (int i = 0; i < 7; ++i) {
    float x = -9.0f + i * 3.0f, r = glm::mix(0.05f, 1.0f, i / 6.0f);
    s.addObject(sphere, trs({x, terrainHeight(x, 0) + 1.0f, 0.0f}, glm::vec3(1.0f)), mat({0.70f, 0.08f, 0.06f}, r, 0.0f));
    s.addObject(sphere, trs({x, terrainHeight(x, -3.5f) + 1.0f, -3.5f}, glm::vec3(1.0f)), mat({1.0f, 0.78f, 0.34f}, r, 1.0f));
  }
  s.addObject(sphere, trs({-15.0f, terrainHeight(-15, -12) + 4.0f, -12.0f}, glm::vec3(4.0f)), mat({0.95f, 0.95f, 0.97f}, 0.02f, 1.0f));

  uint32_t white = mat({0.80f, 0.78f, 0.74f}, 0.6f, 0.0f);
  float cx = 14.0f, cz = -10.0f, cy = terrainHeight(cx, cz);
  s.addObject(cube, trs({cx, cy + 3.0f, cz - 4.0f}, {4.0f, 3.0f, 0.15f}), white);
  s.addObject(cube, trs({cx - 4.0f, cy + 3.0f, cz}, {0.15f, 3.0f, 4.0f}), mat({0.75f, 0.06f, 0.05f}, 0.7f, 0.0f));
  s.addObject(cube, trs({cx + 4.0f, cy + 3.0f, cz}, {0.15f, 3.0f, 4.0f}), mat({0.08f, 0.55f, 0.10f}, 0.7f, 0.0f));
  s.addObject(cube, trs({cx - 1.2f, cy + 1.2f, cz - 1.0f}, {1.0f, 1.2f, 1.0f}, 20.0f), white);
  s.addObject(sphere, trs({cx + 1.6f, cy + 1.1f, cz + 0.5f}, glm::vec3(1.1f)), mat({0.95f, 0.64f, 0.54f}, 0.25f, 1.0f));

  uint32_t stone = mat({0.35f, 0.33f, 0.30f}, 0.85f, 0.0f);
  const glm::vec3 monoliths[] = {{6, 0, -22}, {-4, 0, -30}, {22, 0, -28}, {-26, 0, -4}};
  for (int i = 0; i < 4; ++i) {
    glm::vec3 p = monoliths[i];
    p.y = terrainHeight(p.x, p.z) + 5.0f;
    s.addObject(cube, trs(p, {1.2f, 6.0f, 0.8f}, 25.0f * i), stone);
  }

  // Capacete PBR (texturas reais) num pedestal.
  float hx = 3.0f, hz = 6.0f, hy = terrainHeight(hx, hz);
  s.addObject(cube, trs({hx, hy + 0.5f, hz}, {0.7f, 0.5f, 0.7f}), stone);
  glm::mat4 helmet = glm::translate(glm::mat4(1.0f), {hx, hy + 1.85f, hz});
  helmet = glm::rotate(helmet, glm::radians(-20.0f), glm::vec3(0, 1, 0));
  helmet = glm::scale(helmet, glm::vec3(0.8f));
  loadGltf(s, kAssets + "/DamagedHelmet/DamagedHelmet.gltf", helmet);

  // Postes de luz ao longo de uma "rua" + luzes coloridas espalhadas.
  uint32_t pole = mat({0.05f, 0.05f, 0.05f}, 0.4f, 1.0f);
  uint32_t bulb = mat({1, 1, 1}, 0.5f, 0.0f, {40.0f, 26.0f, 12.0f});
  for (int i = 0; i < 8; ++i) {
    float x = -21.0f + i * 6.0f, z = 9.0f, y = terrainHeight(x, z);
    s.addObject(cube, trs({x, y + 2.5f, z}, {0.06f, 2.5f, 0.06f}), pole);
    s.addObject(sphere, trs({x, y + 5.1f, z}, glm::vec3(0.15f)), bulb);
    s.addLight({{x, y + 5.1f, z}, 0.15f, glm::vec3(30.0f, 19.0f, 8.0f), 25.0f});
  }
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  uint32_t n = opt.lightCount ? opt.lightCount : 128;
  for (uint32_t i = 0; i < n; ++i) {
    float x = glm::mix(-24.0f, 24.0f, u(rng)), z = glm::mix(-16.0f, 12.0f, u(rng));
    glm::vec3 c = hue(u(rng)) * glm::mix(4.0f, 10.0f, u(rng));
    s.addLight({{x, terrainHeight(x, z) + glm::mix(0.3f, 2.0f, u(rng)), z}, 0.05f, c, 8.0f});
  }

  s.sunDirection = sunFrom(16.0f, 55.0f);
  s.sunIntensity = opt.night ? 0.0f : 20.0f;
  camera.position = {2.0f, terrainHeight(2, 16) + 3.2f, 16.0f};
  camera.lookAt({0.0f, 1.0f, -6.0f});
  return true;
}

bool buildSponza(Scene& s, Camera& camera, const DemoOptions& opt) {
  if (!loadGltf(s, kAssets + "/Sponza/Sponza.gltf", glm::mat4(1.0f))) return false;
  glm::vec3 lo, hi;
  s.bounds(lo, hi);
  LOG_INFO("Sponza: limites (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);

  glm::mat4 helmet = glm::translate(glm::mat4(1.0f), {0.0f, lo.y + 1.6f, 0.0f});
  helmet = glm::rotate(helmet, glm::radians(-90.0f), glm::vec3(0, 1, 0));
  loadGltf(s, kAssets + "/DamagedHelmet/DamagedHelmet.gltf", helmet);

  // Centenas de luzes coloridas pelo átrio: o caso de uso do ReSTIR.
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  uint32_t n = opt.lightCount ? opt.lightCount : 512;
  glm::vec3 inner = glm::mix(lo, hi, 0.08f), outer = glm::mix(lo, hi, 0.92f);
  for (uint32_t i = 0; i < n; ++i) {
    glm::vec3 p(glm::mix(inner.x, outer.x, u(rng)), glm::mix(lo.y + 0.3f, lo.y + 0.75f * (hi.y - lo.y), u(rng)),
                glm::mix(inner.z, outer.z, u(rng)));
    glm::vec3 c = hue(u(rng)) * glm::mix(0.6f, 2.0f, u(rng));
    s.addLight({p, 0.04f, c, 5.0f});
  }

  s.sunDirection = sunFrom(65.0f, 100.0f);
  s.sunIntensity = opt.night ? 0.0f : 20.0f;
  // Do fundo do átrio olhando ao longo do eixo X (o comprido).
  camera.position = {-8.5f, 1.6f, -0.6f};
  camera.lookAt({5.0f, 3.4f, 0.4f});
  return true;
}

}  // namespace

bool buildDemoScene(Scene& scene, Camera& camera, const DemoOptions& opt) {
  if (opt.name == "outdoor") return buildOutdoor(scene, camera, opt);
  return buildSponza(scene, camera, opt);
}

}  // namespace scene
