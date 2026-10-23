#include "scene/demo_scene.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <random>

#include <glm/gtc/constants.hpp>
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
// O CesiumMan é exportado olhando para +X; gira -90° para alinhar a frente com +Z.
const float kCesiumManFacing = -glm::half_pi<float>();

// Carrega um personagem animado com a escala dada; devolve o modelo e o deslocamento que põe os pés no chão.
int loadCharacter(Scene& s, const std::string& file, float scale, const std::string& clip, float& footOffset) {
  int model = -1;
  if (!loadGltf(s, kAssets + "/" + file, glm::scale(glm::mat4(1.0f), glm::vec3(scale)), &model) || model < 0) return -1;
  Model& m = s.models[size_t(model)];
  int a = m.findAnimation(clip);
  m.animation = a >= 0 ? a : 0;
  // Pose inicial para medir a altura dos pés.
  s.update(0.0f);
  glm::vec3 lo, hi;
  s.modelBounds(model, lo, hi);
  footOffset = -lo.y;
  return model;
}

// Faz um modelo percorrer uma elipse no plano XZ, virado na direção do movimento.
// facingYaw: rotação extra para alinhar a "frente" do modelo com +Z.
void walkEllipse(Scene& s, int model, glm::vec3 center, glm::vec2 radii, float speed, float scale, float footOffset,
                 float facingYaw, float phase, std::function<float(float, float)> ground) {
  if (model < 0) return;
  float meanRadius = 0.5f * (radii.x + radii.y);
  s.addAnimator([=](Scene& sc, float t, float) {
    float th = phase + speed / meanRadius * t;
    glm::vec3 p = center + glm::vec3(radii.x * std::cos(th), 0.0f, radii.y * std::sin(th));
    glm::vec3 v(-radii.x * std::sin(th), 0.0f, radii.y * std::cos(th));
    if (ground) p.y = ground(p.x, p.z);
    float yaw = std::atan2(v.x, v.z) + facingYaw;
    glm::mat4 m = glm::translate(glm::mat4(1.0f), p + glm::vec3(0.0f, footOffset, 0.0f));
    m = glm::rotate(m, yaw, glm::vec3(0, 1, 0));
    sc.models[size_t(model)].root = glm::scale(m, glm::vec3(scale));
  });
}

// Luzes coloridas orbitando um ponto, cada uma com uma pequena esfera emissiva.
void addOrbitingLights(Scene& s, uint32_t sphereMesh, glm::vec3 center, float radius, int count, float intensity) {
  for (int i = 0; i < count; ++i) {
    glm::vec3 color = hue(float(i) / float(count));
    Material m;
    m.baseColor = glm::vec4(color, 1.0f);
    m.emissive = color * 30.0f;
    uint32_t object = s.addObject(sphereMesh, glm::mat4(1.0f), s.addMaterial(m));
    uint32_t light = s.addLight({center, 0.08f, color * intensity, 12.0f});
    float phase = glm::two_pi<float>() * float(i) / float(count);
    s.addAnimator([=](Scene& sc, float t, float) {
      float a = phase + 0.8f * t;
      glm::vec3 p = center + glm::vec3(radius * std::cos(a), 0.4f * std::sin(2.0f * a), radius * std::sin(a));
      sc.objects[object].transform = glm::scale(glm::translate(glm::mat4(1.0f), p), glm::vec3(0.08f));
      sc.lights[light].position = p;
    });
  }
}

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

  // ---- Conteúdo dinâmico ----
  float foxFoot = 0, manFoot = 0;
  int fox = loadCharacter(s, "Fox/Fox.gltf", 0.012f, "Run", foxFoot);
  walkEllipse(s, fox, {0.0f, 0.0f, 4.0f}, {7.0f, 3.0f}, 5.0f, 0.012f, foxFoot, 0.0f, 0.0f, terrainHeight);
  int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", manFoot);
  walkEllipse(s, man, {-5.0f, 0.0f, 6.0f}, {2.5f, 2.5f}, 1.2f, 1.0f, manFoot, kCesiumManFacing, 1.0f, terrainHeight);
  addOrbitingLights(s, sphere, {hx, hy + 2.0f, hz}, 1.4f, 3, 6.0f);

  // Carro: carroceria, cabine, rodas e faróis (luzes que se movem com ele).
  uint32_t paint = mat({0.02f, 0.05f, 0.25f}, 0.25f, 1.0f);
  uint32_t glass = mat({0.02f, 0.02f, 0.02f}, 0.05f, 0.0f);
  uint32_t tire = mat({0.03f, 0.03f, 0.03f}, 0.9f, 0.0f);
  uint32_t lamp = mat({1, 1, 1}, 0.3f, 0.0f, {60.0f, 55.0f, 45.0f});
  struct Part { uint32_t object; glm::mat4 local; };
  std::vector<Part> car;
  auto part = [&](uint32_t mesh, uint32_t material, glm::vec3 t, glm::vec3 sc) {
    glm::mat4 local = glm::scale(glm::translate(glm::mat4(1.0f), t), sc);
    car.push_back({s.addObject(mesh, local, material), local});
  };
  part(cube, paint, {0, 0.55f, 0}, {0.95f, 0.32f, 2.2f});
  part(cube, glass, {0, 1.05f, -0.2f}, {0.8f, 0.25f, 1.1f});
  for (float x : {-0.95f, 0.95f})
    for (float z : {-1.4f, 1.4f}) part(sphere, tire, {x, 0.36f, z}, {0.14f, 0.36f, 0.36f});
  std::vector<std::pair<uint32_t, glm::vec3>> headlights;
  for (float x : {-0.6f, 0.6f}) {
    glm::vec3 local(x, 0.6f, 2.25f);
    part(sphere, lamp, local, glm::vec3(0.1f));
    headlights.push_back({s.addLight({glm::vec3(0.0f), 0.1f, glm::vec3(40.0f, 36.0f, 30.0f), 30.0f}), local});
  }
  s.addAnimator([=](Scene& sc, float t, float) {
    const float radius = 20.0f, speed = 12.0f;  // ~43 km/h
    float th = speed / radius * t;
    glm::vec3 p(radius * std::cos(th), 0.0f, -4.0f + radius * std::sin(th));
    p.y = terrainHeight(p.x, p.z);
    glm::vec3 v(-std::sin(th), 0.0f, std::cos(th));
    glm::mat4 body = glm::rotate(glm::translate(glm::mat4(1.0f), p), std::atan2(v.x, v.z), glm::vec3(0, 1, 0));
    for (const Part& pt : car) sc.objects[pt.object].transform = body * pt.local;
    for (auto [light, local] : headlights) sc.lights[light].position = glm::vec3(body * glm::vec4(local, 1.0f));
  });

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

  // ---- Conteúdo dinâmico: personagens andando pelo átrio e lanternas orbitando o capacete ----
  float foxFoot = 0, manFoot = 0;
  int fox = loadCharacter(s, "Fox/Fox.gltf", 0.01f, "Walk", foxFoot);
  walkEllipse(s, fox, {0.0f, lo.y, 0.0f}, {7.0f, 1.6f}, 1.6f, 0.01f, foxFoot, 0.0f, 0.0f, nullptr);
  int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", manFoot);
  walkEllipse(s, man, {0.0f, lo.y, 0.0f}, {5.0f, 1.2f}, 1.2f, 1.0f, manFoot, kCesiumManFacing, 3.0f, nullptr);
  addOrbitingLights(s, s.addMesh(makeSphere(32, 16)), {0.0f, lo.y + 1.6f, 0.0f}, 1.5f, 4, 4.0f);

  s.sunDirection = sunFrom(65.0f, 100.0f);
  s.sunIntensity = opt.night ? 0.0f : 20.0f;
  // Do fundo do átrio olhando ao longo do eixo X (o comprido).
  camera.position = {-8.5f, 1.6f, -0.6f};
  camera.lookAt({5.0f, 3.4f, 0.4f});
  return true;
}

}  // namespace

// Qualquer arquivo glTF/GLB: câmera posicionada pela caixa envolvente.
bool buildFromFile(Scene& s, Camera& camera, const DemoOptions& opt) {
  if (!loadGltf(s, opt.name, glm::mat4(1.0f))) return false;
  glm::vec3 lo, hi;
  s.bounds(lo, hi);
  LOG_INFO("Arquivo: limites (%.1f %.1f %.1f) .. (%.1f %.1f %.1f)", lo.x, lo.y, lo.z, hi.x, hi.y, hi.z);
  glm::vec3 c = 0.5f * (lo + hi);
  camera.position = c + glm::vec3(0.0f, 0.1f * (hi.y - lo.y), 0.0f);
  camera.lookAt(c + glm::vec3(hi.x - lo.x, 0.0f, 0.0f));
  s.sunDirection = sunFrom(35.0f, 120.0f);
  s.sunIntensity = opt.night ? 0.0f : 20.0f;
  return true;
}

bool buildDemoScene(Scene& scene, Camera& camera, const DemoOptions& opt) {
  auto endsWith = [&](const char* ext) {
    size_t n = std::strlen(ext);
    return opt.name.size() > n && opt.name.compare(opt.name.size() - n, n, ext) == 0;
  };
  if (endsWith(".gltf") || endsWith(".glb")) return buildFromFile(scene, camera, opt);
  if (opt.name == "outdoor") return buildOutdoor(scene, camera, opt);
  return buildSponza(scene, camera, opt);
}

}  // namespace scene
