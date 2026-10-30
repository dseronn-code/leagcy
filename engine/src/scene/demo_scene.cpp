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
    m.emissive = color * 9000.0f;  // nits
    m.flags |= kMaterialLightGeometry | kMaterialEmissionSampled;  // a esfera é a forma visível da luz abaixo
    uint32_t object = s.addObject(sphereMesh, glm::mat4(1.0f), s.addMaterial(m));
    uint32_t light = s.addLight({center, 0.08f, color * intensity, 12.0f});  // cd
    float phase = glm::two_pi<float>() * float(i) / float(count);
    s.addAnimator([=](Scene& sc, float t, float) {
      float a = phase + 0.8f * t;
      glm::vec3 p = center + glm::vec3(radius * std::cos(a), 0.4f * std::sin(2.0f * a), radius * std::sin(a));
      sc.setTransform(object, glm::scale(glm::translate(glm::mat4(1.0f), p), glm::vec3(0.08f)));
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
  // Grama: 4 variações de tufo instanciadas aos milhares na encosta perto da câmera (uma chamada por variação; no
  // ray tracing, uma BLAS por variação). Balança com o vento (folhagem).
  {
    Material grass;
    grass.baseColor = glm::vec4(0.16f, 0.24f, 0.06f, 1.0f);
    grass.roughness = 0.6f;
    grass.flags = kMaterialDoubleSided | kMaterialFoliage;
    const uint32_t grassMat = s.addMaterial(grass);
    uint32_t clumps[4];
    for (uint32_t v = 0; v < 4; ++v) clumps[v] = s.addMesh(makeGrassClump(14, 0.45f, v + 1));
    std::mt19937 grng(99);
    std::uniform_real_distribution<float> gu(0.0f, 1.0f);
    for (int i = 0; i < 6000; ++i) {
      const float x = -30.0f + 60.0f * gu(grng), z = -12.0f + 45.0f * gu(grng);
      if (std::abs(z - 9.0f) < 1.5f) continue;  // a "rua" dos postes fica livre
      const float y = terrainHeight(x, z);
      glm::mat4 t = glm::translate(glm::mat4(1.0f), {x, y - 0.02f, z});
      t = glm::rotate(t, glm::two_pi<float>() * gu(grng), glm::vec3(0, 1, 0));
      t = glm::scale(t, glm::vec3(0.7f + 0.6f * gu(grng)));
      s.addObject(clumps[i % 4], t, grassMat);
    }
  }

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
  uint32_t bulb = mat({1, 1, 1}, 0.5f, 0.0f, {40000.0f, 26000.0f, 12000.0f});  // globo de vapor de sódio (nits)
  s.materials[bulb].flags |= kMaterialLightGeometry | kMaterialEmissionSampled;  // globo = forma da luz do poste
  for (int i = 0; i < 8; ++i) {
    float x = -21.0f + i * 6.0f, z = 9.0f, y = terrainHeight(x, z);
    s.addObject(cube, trs({x, y + 2.5f, z}, {0.06f, 2.5f, 0.06f}), pole);
    s.addObject(sphere, trs({x, y + 5.1f, z}, glm::vec3(0.15f)), bulb);
    s.addLight({{x, y + 5.1f, z}, 0.15f, glm::vec3(1800.0f, 1140.0f, 480.0f), 25.0f});  // poste de rua (cd)
  }
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  uint32_t n = opt.lightCount ? opt.lightCount : 128;
  for (uint32_t i = 0; i < n; ++i) {
    float x = glm::mix(-24.0f, 24.0f, u(rng)), z = glm::mix(-16.0f, 12.0f, u(rng));
    glm::vec3 c = hue(u(rng)) * glm::mix(60.0f, 150.0f, u(rng));  // luzes decorativas (cd)
    s.addLight({{x, terrainHeight(x, z) + glm::mix(0.3f, 2.0f, u(rng)), z}, 0.05f, c, 8.0f});
  }

  // ---- Conteúdo dinâmico ----
  float foxFoot = 0, manFoot = 0;
  int fox = loadCharacter(s, "Fox/Fox.gltf", 0.012f, "Run", foxFoot);
  walkEllipse(s, fox, {0.0f, 0.0f, 4.0f}, {7.0f, 3.0f}, 5.0f, 0.012f, foxFoot, 0.0f, 0.0f, terrainHeight);
  int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", manFoot);
  walkEllipse(s, man, {-5.0f, 0.0f, 6.0f}, {2.5f, 2.5f}, 1.2f, 1.0f, manFoot, kCesiumManFacing, 1.0f, terrainHeight);
  addOrbitingLights(s, sphere, {hx, hy + 2.0f, hz}, 1.4f, 3, 240.0f);

  // Carro: carroceria, cabine, rodas e faróis (luzes que se movem com ele).
  uint32_t paint = mat({0.02f, 0.05f, 0.25f}, 0.25f, 1.0f);
  s.materials[paint].flags |= kMaterialClearcoat;  // pintura de carro: metálico sob verniz
  s.materials[paint].clearcoat = 1.0f;
  s.materials[paint].clearcoatRoughness = 0.03f;
  uint32_t glass = mat({0.02f, 0.02f, 0.02f}, 0.05f, 0.0f);
  uint32_t tire = mat({0.03f, 0.03f, 0.03f}, 0.9f, 0.0f);
  uint32_t lamp = mat({1, 1, 1}, 0.3f, 0.0f, {60000.0f, 55000.0f, 45000.0f});  // farol (nits)
  s.materials[lamp].flags |= kMaterialLightGeometry | kMaterialEmissionSampled;  // forma visível do farol
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
    headlights.push_back({s.addLight({glm::vec3(0.0f), 0.1f, glm::vec3(3000.0f, 2700.0f, 2250.0f), 30.0f}), local});  // cd
  }
  s.addAnimator([=](Scene& sc, float t, float) {
    const float radius = 20.0f, speed = 12.0f;  // ~43 km/h
    float th = speed / radius * t;
    glm::vec3 p(radius * std::cos(th), 0.0f, -4.0f + radius * std::sin(th));
    p.y = terrainHeight(p.x, p.z);
    glm::vec3 v(-std::sin(th), 0.0f, std::cos(th));
    glm::mat4 body = glm::rotate(glm::translate(glm::mat4(1.0f), p), std::atan2(v.x, v.z), glm::vec3(0, 1, 0));
    for (const Part& pt : car) sc.setTransform(pt.object, body * pt.local);
    for (auto [light, local] : headlights) sc.lights[light].position = glm::vec3(body * glm::vec4(local, 1.0f));
  });

  s.sunDirection = sunFrom(16.0f, 55.0f);
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.cloudCoverage = 0.4f;  // céu com cúmulos
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;  // lua cheia
  // Bandeira de tecido simulado num mastro (3 x 2 m, 30 x 20 vértices).
  {
    const float fx = 6.0f, fz = 9.0f, fy = terrainHeight(fx, fz);
    s.addObject(cube, trs({fx, fy + 3.5f, fz}, {0.05f, 3.5f, 0.05f}), mat({0.6f, 0.6f, 0.62f}, 0.3f, 1.0f));
    Material cloth;
    cloth.baseColor = glm::vec4(0.7f, 0.08f, 0.06f, 1.0f);
    cloth.roughness = 0.85f;
    cloth.flags = kMaterialDoubleSided;
    s.addObject(s.addMesh(makeClothGrid(30, 20, 0.1f)), glm::translate(glm::mat4(1.0f), {fx + 0.06f, fy + 6.8f, fz}),
                s.addMaterial(cloth));
  }
  // Fogueira: chama, fumaça e faíscas (partículas) com a luz que tremula.
  {
    const glm::vec3 fire(-3.0f, terrainHeight(-3.0f, 11.0f), 11.0f);
    s.addEmitter(kParticleFire, fire, 0.35f, 1.0f, 900);
    s.addEmitter(kParticleSmoke, fire, 0.3f, 1.0f, 500);
    s.addEmitter(kParticleSparks, fire, 0.2f, 1.0f, 150);
  }
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
    glm::vec3 c = hue(u(rng)) * glm::mix(36.0f, 120.0f, u(rng));  // cd
    s.addLight({p, 0.04f, c, 5.0f});
  }

  // ---- Conteúdo dinâmico: personagens andando pelo átrio e lanternas orbitando o capacete ----
  float foxFoot = 0, manFoot = 0;
  int fox = loadCharacter(s, "Fox/Fox.gltf", 0.01f, "Walk", foxFoot);
  walkEllipse(s, fox, {0.0f, lo.y, 0.0f}, {7.0f, 1.6f}, 1.6f, 0.01f, foxFoot, 0.0f, 0.0f, nullptr);
  int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", manFoot);
  walkEllipse(s, man, {0.0f, lo.y, 0.0f}, {5.0f, 1.2f}, 1.2f, 1.0f, manFoot, kCesiumManFacing, 3.0f, nullptr);
  addOrbitingLights(s, s.addMesh(makeSphere(32, 16)), {0.0f, lo.y + 1.6f, 0.0f}, 1.5f, 4, 160.0f);

  s.sunDirection = sunFrom(65.0f, 100.0f);
  s.fogDensity = 0.0001f;  // átrio: névoa sem sombra volumétrica viraria um véu branco nas áreas de sombra
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;  // lua cheia
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
  if (s.assetCamera.valid) {  // vista do autor do asset
    camera.position = s.assetCamera.position;
    camera.lookAt(s.assetCamera.position + s.assetCamera.forward);
    camera.fovY = s.assetCamera.fovY;
    camera.nearPlane = s.assetCamera.nearPlane;
  }
  if (!s.sunFromAsset) s.sunDirection = sunFrom(35.0f, 120.0f);
  s.cloudCoverage = 0.4f;  // céu com cúmulos
  s.fogDensity = 0.0003f;
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;  // lua cheia
  return true;
}

// Teste da fornalha branca: esferas brancas sob céu uniforme. Um BRDF que conserva energia
// as deixa com o mesmo brilho do fundo (metais f0 = 1 em cima, dielétricos brancos embaixo,
// rugosidade de 0 a 1 da esquerda para a direita).
bool buildFurnace(Scene& s, Camera& camera) {
  uint32_t sphere = s.addMesh(makeSphere(128, 64));
  for (int row = 0; row < 2; ++row) {
    for (int i = 0; i < 6; ++i) {
      Material m;
      m.baseColor = glm::vec4(1.0f);
      m.metallic = row == 0 ? 1.0f : 0.0f;
      m.roughness = float(i) / 5.0f;
      s.addObject(sphere, trs({-6.25f + 2.5f * i, row == 0 ? 1.3f : -1.3f, 0.0f}, glm::vec3(1.0f)), s.addMaterial(m));
    }
  }
  s.sunIlluminance = 0.0f;
  s.fogDensity = 0.0f;
  s.uniformSkyLuminance = 1.0f;
  camera.position = {0.0f, 0.0f, 13.0f};
  camera.lookAt({0.0f, 0.0f, 0.0f});
  return true;
}

// Teste de sombras do sol: esferas de baixa a alta resolução com normais suavizadas (problema do
// "terminator": a faceta real faz sombra onde a normal interpolada diz que é luz), placa fina deitada
// no chão (sombra de contato, sem "peter panning"), caixas de quinas vivas (acne em luz rasante).
// `origin` desloca tudo (ex.: 4 km) para testar precisão de float longe da origem, como num mapa aberto.
bool buildShadowTest(Scene& s, Camera& camera, glm::vec3 origin) {
  uint32_t cube = s.addMesh(makeCube());
  auto mat = [&](glm::vec3 albedo, float rough) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    return s.addMaterial(m);
  };
  uint32_t grey = mat(glm::vec3(0.5f), 0.8f), white = mat(glm::vec3(0.8f), 0.5f);
  s.addObject(cube, trs(origin + glm::vec3(0.0f, -0.5f, 0.0f), {60.0f, 0.5f, 60.0f}), grey);  // chão, topo em y = 0
  const uint32_t res[][2] = {{6, 4}, {8, 6}, {12, 8}, {16, 10}, {24, 16}, {64, 32}};
  for (int i = 0; i < 6; ++i)
    s.addObject(s.addMesh(makeSphere(res[i][0], res[i][1])), trs(origin + glm::vec3(-7.5f + 3.0f * i, 1.4f, 0.0f), glm::vec3(1.0f)), white);
  s.addObject(cube, trs(origin + glm::vec3(-4.0f, 0.01f, 4.0f), {1.0f, 0.01f, 1.0f}), white);  // placa de 2 cm no chão
  s.addObject(cube, trs(origin + glm::vec3(1.0f, 0.5f, 4.5f), {0.5f, 0.5f, 0.5f}, 30.0f), white);
  s.addObject(cube, trs(origin + glm::vec3(5.0f, 1.0f, 3.5f), {0.3f, 1.0f, 1.5f}), white);
  s.sunDirection = sunFrom(20.0f, 45.0f);  // lateral e por trás: terminator no meio das esferas; a fila não se sombreia
  s.sunIlluminance = 120000.0f;
  s.fogDensity = 0.0f;
  camera.position = origin + glm::vec3(0.0f, 3.0f, 13.0f);
  camera.lookAt(origin + glm::vec3(0.0f, 0.8f, 0.0f));
  return true;
}

// Cidade procedural para medir escala (Fase 2): grade de quarteirões de 60 m com prédios de altura aleatória (concreto
// ou vidro), calçada, postes com luz de verdade a cada ~20 m e carros estacionados. Poucas malhas, muitas instâncias
// (como uma cidade de jogo): o custo de objetos, BLAS/TLAS, culling e luzes cresce com `cityBlocks`².
bool buildCity(Scene& s, Camera& camera, const DemoOptions& opt) {
  const uint32_t cube = s.addMesh(makeCube()), sphere = s.addMesh(makeSphere(12, 8));
  auto mat = [&](glm::vec3 albedo, float rough, float metal, glm::vec3 emissive = glm::vec3(0.0f)) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    m.emissive = emissive;
    return s.addMaterial(m);
  };
  const uint32_t asphalt = mat({0.06f, 0.06f, 0.065f}, 0.85f, 0.0f), sidewalk = mat({0.42f, 0.40f, 0.37f}, 0.9f, 0.0f);
  const uint32_t pole = mat({0.05f, 0.05f, 0.05f}, 0.4f, 1.0f);
  const uint32_t bulb = mat({1, 1, 1}, 0.5f, 0.0f, {40000.0f, 30000.0f, 18000.0f});
  s.materials[bulb].flags |= kMaterialLightGeometry | kMaterialEmissionSampled;  // globo = forma da luz do poste
  uint32_t concrete[4], glass[3], paint[5];
  const glm::vec3 concreteColors[4] = {{0.55f, 0.52f, 0.48f}, {0.62f, 0.58f, 0.50f}, {0.40f, 0.38f, 0.36f}, {0.70f, 0.66f, 0.60f}};
  for (int i = 0; i < 4; ++i) concrete[i] = mat(concreteColors[i], 0.8f, 0.0f);
  const glm::vec3 glassColors[3] = {{0.55f, 0.65f, 0.70f}, {0.70f, 0.70f, 0.72f}, {0.45f, 0.55f, 0.50f}};
  for (int i = 0; i < 3; ++i) glass[i] = mat(glassColors[i], 0.08f, 1.0f);  // fachada de vidro espelhado
  const glm::vec3 paintColors[5] = {{0.6f, 0.05f, 0.04f}, {0.05f, 0.12f, 0.45f}, {0.8f, 0.8f, 0.8f}, {0.02f, 0.02f, 0.02f}, {0.7f, 0.55f, 0.1f}};
  for (int i = 0; i < 5; ++i) {
    paint[i] = mat(paintColors[i], 0.35f, 0.3f);  // pintura de carro: base levemente metálica sob verniz
    s.materials[paint[i]].flags |= kMaterialClearcoat;
    s.materials[paint[i]].clearcoat = 1.0f;
    s.materials[paint[i]].clearcoatRoughness = 0.03f;
  }

  const uint32_t n = std::max(1u, opt.cityBlocks);
  const float block = 60.0f, street = 14.0f, pitch = block + street, half = 0.5f * float(n) * pitch;
  s.addObject(cube, trs({0.0f, -0.5f, 0.0f}, {half + 50.0f, 0.5f, half + 50.0f}), asphalt);  // ruas, topo em y = 0
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  for (uint32_t bz = 0; bz < n; ++bz)
    for (uint32_t bx = 0; bx < n; ++bx) {
      const float cx = -half + pitch * (float(bx) + 0.5f), cz = -half + pitch * (float(bz) + 0.5f);
      s.addObject(cube, trs({cx, 0.08f, cz}, {0.5f * block, 0.08f, 0.5f * block}), sidewalk);
      // Sinalização e desgaste nas ruas do centro (decals procedurais): linha tracejada no meio de cada rua, faixa de
      // pedestre antes do cruzamento, sujeira na calçada e manchas de óleo onde os carros param.
      if (std::abs(int(bx) - int(n / 2)) <= 2 && std::abs(int(bz) - int(n / 2)) <= 2) {
        const float ex = cx + 0.5f * pitch, ez = cz + 0.5f * pitch;  // ruas a leste e ao sul do quarteirão
        const glm::vec4 white(0.95f, 0.95f, 0.92f, 1.0f), yellow(0.95f, 0.75f, 0.15f, 1.0f);
        s.addDecal(kDecalLaneLine, {ex, 0.0f, cz}, {0.08f, 0.5f, 0.5f * block}, 0.0f, yellow);
        s.addDecal(kDecalLaneLine, {cx, 0.0f, ez}, {0.08f, 0.5f, 0.5f * block}, glm::half_pi<float>(), yellow);
        s.addDecal(kDecalCrosswalk, {ex, 0.0f, cz + 0.5f * block - 3.0f}, {0.5f * street - 0.5f, 0.5f, 2.0f}, 0.0f, white);
        s.addDecal(kDecalCrosswalk, {cx + 0.5f * block - 3.0f, 0.0f, ez}, {0.5f * street - 0.5f, 0.5f, 2.0f}, glm::half_pi<float>(), white);
        s.addDecal(kDecalDirt, {cx, 0.16f, cz}, {0.5f * block, 0.5f, 0.5f * block}, 0.0f, glm::vec4(1.0f));
        const float jitter = std::fmod(std::abs(std::sin(float(bx) * 12.9898f + float(bz) * 78.233f)) * 43758.55f, 1.0f);  // não mexe no rng
        s.addDecal(kDecalOil, {ex - 3.5f, 0.0f, cz + 10.0f * (jitter - 0.5f)}, {0.9f, 0.5f, 1.4f}, jitter * 3.0f, glm::vec4(1.0f));
      }
      // Prédios: 2x3 lotes por quarteirão, alturas de 8 a 90 m (mais altos perto do centro da cidade).
      const float centrality = 1.0f - glm::length(glm::vec2(cx, cz)) / (half * 1.42f);
      for (int lz = 0; lz < 3; ++lz)
        for (int lx = 0; lx < 2; ++lx) {
          if (u(rng) < 0.12f) continue;  // lote vazio (praça)
          const float w = 11.0f + 7.0f * u(rng), d = 8.0f + 6.0f * u(rng);
          const float h = 8.0f + (20.0f + 62.0f * centrality * centrality) * u(rng);
          const float px = cx + (lx == 0 ? -1.0f : 1.0f) * 14.0f, pz = cz + (float(lz) - 1.0f) * 18.0f;
          const uint32_t m = u(rng) < 0.3f ? glass[rng() % 3] : concrete[rng() % 4];
          s.addObject(cube, trs({px, 0.16f + 0.5f * h, pz}, {0.5f * w, 0.5f * h, 0.5f * d}), m);
        }
      // Postes nas duas calçadas voltadas para as ruas (a cada 20 m), com luz de vapor de sódio.
      for (int i = 0; i < 3; ++i)
        for (int side = 0; side < 2; ++side) {
          const float x = cx - 20.0f + 20.0f * float(i), z = cz + (side == 0 ? -1.0f : 1.0f) * (0.5f * block - 1.0f);
          s.addObject(cube, trs({x, 3.0f, z}, {0.07f, 3.0f, 0.07f}), pole);
          s.addObject(sphere, trs({x, 6.1f, z}, glm::vec3(0.18f)), bulb);
          s.addLight({{x, 6.1f, z}, 0.18f, glm::vec3(2200.0f, 1600.0f, 900.0f), 28.0f});
        }
      // Carros estacionados na rua ao lado do quarteirão.
      for (int i = 0; i < 4; ++i) {
        if (u(rng) < 0.3f) continue;
        const float x = cx - 24.0f + 16.0f * float(i), z = cz + 0.5f * block + 2.6f;
        const uint32_t m = paint[rng() % 5];
        s.addObject(cube, trs({x, 0.75f, z}, {2.2f, 0.45f, 0.95f}), m);           // carroceria
        s.addObject(cube, trs({x - 0.3f, 1.45f, z}, {1.2f, 0.3f, 0.85f}), m);     // cabine
      }
    }
  s.sunDirection = sunFrom(opt.night ? -10.0f : 35.0f, 140.0f);
  s.sunIlluminance = opt.night ? 0.0f : 100000.0f;
  s.cloudCoverage = 0.4f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;  // lua cheia
  s.fogDensity = 0.0f;
  // Na rua, entre dois quarteirões perto do centro, olhando ao longo da avenida.
  camera.position = glm::vec3(-0.5f * street, 1.7f, 0.25f * pitch);
  camera.lookAt(glm::vec3(-0.5f * street, 6.0f, -half));
  return true;
}

// Cena de teste dos reflexos (tools/reflexos.py): sala aberta na frente, com piso de espelho (cromo, rugosidade
// 0,02), paredes coloridas e quatro esferas de ouro de rugosidade 0 / 0,15 / 0,35 / 0,6. O sol entra baixo pela
// abertura e bate só na parte de cima da parede do fundo (não no espelho: sol refletido por espelho é cáustica, que
// motor de jogo nenhum calcula e o limite de vagalumes corta); o resto da sala só recebe luz rebatida — é onde um
// reflexo que ilumina o ponto atingido com "céu sem oclusão" erra, e o espelho mostra os reflexos dos reflexos.
bool buildReflectionTest(Scene& s, Camera& camera) {
  uint32_t cube = s.addMesh(makeCube());
  uint32_t sphere = s.addMesh(makeSphere(64, 32));
  auto mat = [&](glm::vec3 albedo, float rough, float metal) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    return s.addMaterial(m);
  };
  const uint32_t ground = mat(glm::vec3(0.4f), 0.9f, 0.0f), white = mat(glm::vec3(0.75f), 0.8f, 0.0f);
  const uint32_t red = mat(glm::vec3(0.7f, 0.08f, 0.06f), 0.7f, 0.0f), green = mat(glm::vec3(0.08f, 0.55f, 0.12f), 0.7f, 0.0f);
  const uint32_t chrome = mat(glm::vec3(0.95f), 0.02f, 1.0f);
  s.addObject(cube, trs({0.0f, -0.5f, 10.0f}, {40.0f, 0.5f, 30.0f}), ground);  // chão de fora, topo em y = 0
  s.addObject(cube, trs({0.0f, 0.01f, -4.0f}, {4.0f, 0.01f, 4.0f}), chrome);   // piso da sala: espelho
  s.addObject(cube, trs({0.0f, 2.0f, -8.1f}, {4.2f, 2.0f, 0.1f}), red);        // fundo
  s.addObject(cube, trs({-4.1f, 2.0f, -4.0f}, {0.1f, 2.0f, 4.2f}), green);     // esquerda
  s.addObject(cube, trs({4.1f, 2.0f, -4.0f}, {0.1f, 2.0f, 4.2f}), white);      // direita
  s.addObject(cube, trs({0.0f, 4.1f, -4.0f}, {4.2f, 0.1f, 4.2f}), white);      // teto
  const float rough[] = {0.0f, 0.15f, 0.35f, 0.6f};
  for (int i = 0; i < 4; ++i)
    s.addObject(sphere, trs({-2.4f + 1.6f * float(i), 0.62f, -4.5f}, glm::vec3(0.6f)), mat({1.0f, 0.78f, 0.34f}, rough[i], 1.0f));
  s.sunDirection = sunFrom(18.0f, 168.0f);  // pela abertura da frente: mancha de sol no alto da parede do fundo
  s.sunIlluminance = 100000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 2.2f, 4.0f);
  camera.lookAt(glm::vec3(0.0f, 1.0f, -5.0f));
  return true;
}

// Teste das luzes de malha: sala fechada (sem sol) iluminada só por emissivos — painel no teto, globos coloridos e o
// capacete (textura emissiva). Comparada por path tracing com e sem --no-mesh-lights: devem convergir para o mesmo.
bool buildMeshLightTest(Scene& s, Camera& camera) {
  uint32_t cube = s.addMesh(makeCube());
  uint32_t sphere = s.addMesh(makeSphere(48, 24));
  auto mat = [&](glm::vec3 albedo, float rough, float metal, glm::vec3 emissive = glm::vec3(0.0f)) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.metallic = metal;
    m.emissive = emissive;
    return s.addMaterial(m);
  };
  const uint32_t floor = mat(glm::vec3(0.5f), 0.35f, 0.0f), wall = mat(glm::vec3(0.7f), 0.9f, 0.0f);
  const uint32_t red = mat(glm::vec3(0.7f, 0.08f, 0.06f), 0.8f, 0.0f), metal = mat(glm::vec3(0.9f), 0.25f, 1.0f);
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {4.0f, 0.1f, 4.0f}), floor);
  s.addObject(cube, trs({0.0f, 3.1f, 0.0f}, {4.0f, 0.1f, 4.0f}), wall);
  s.addObject(cube, trs({0.0f, 1.5f, -4.1f}, {4.0f, 1.6f, 0.1f}), red);
  s.addObject(cube, trs({0.0f, 1.5f, 4.1f}, {4.0f, 1.6f, 0.1f}), wall);
  s.addObject(cube, trs({-4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  s.addObject(cube, trs({4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  s.addObject(cube, trs({0.0f, 2.99f, -1.0f}, {0.8f, 0.01f, 0.4f}), mat(glm::vec3(1.0f), 0.5f, 0.0f, glm::vec3(3000.0f)));
  s.addObject(sphere, trs({-2.0f, 0.15f, 0.5f}, glm::vec3(0.15f)), mat(glm::vec3(1.0f), 0.5f, 0.0f, {20000.0f, 6000.0f, 1500.0f}));
  s.addObject(sphere, trs({2.2f, 1.2f, -2.0f}, glm::vec3(0.1f)), mat(glm::vec3(1.0f), 0.5f, 0.0f, {2000.0f, 9000.0f, 30000.0f}));
  s.addObject(sphere, trs({1.2f, 0.5f, 0.6f}, glm::vec3(0.5f)), metal);
  glm::mat4 helmet = glm::translate(glm::mat4(1.0f), {-0.6f, 1.0f, -1.5f});
  loadGltf(s, kAssets + "/DamagedHelmet/DamagedHelmet.gltf", glm::scale(helmet, glm::vec3(0.6f)));
  s.sunIlluminance = 0.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 1.6f, 3.6f);
  camera.lookAt(glm::vec3(0.0f, 1.0f, -1.5f));
  return true;
}

// Teste do vidro: sala com janela de vidro claro, painel de vidro colorido, lanterna (globo de vidro com uma lâmpada
// dentro) e uma janela dupla (duas lâminas). Sol entrando pela janela.
bool buildGlassTest(Scene& s, Camera& camera) {
  uint32_t cube = s.addMesh(makeCube());
  uint32_t sphere = s.addMesh(makeSphere(48, 24));
  auto mat = [&](glm::vec3 albedo, float rough, float transmission = 0.0f, glm::vec3 emissive = glm::vec3(0.0f)) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.emissive = emissive;
    if (transmission > 0.0f) {
      m.transmission = transmission;
      m.flags |= kMaterialTransmissive;
    }
    return s.addMaterial(m);
  };
  const uint32_t floor = mat(glm::vec3(0.45f), 0.6f), wall = mat(glm::vec3(0.7f), 0.9f);
  const uint32_t clear = mat(glm::vec3(1.0f), 0.0f, 0.95f), tinted = mat({0.2f, 0.6f, 1.0f}, 0.05f, 0.9f);
  const uint32_t frosted = mat(glm::vec3(1.0f), 0.35f, 0.9f);
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {6.0f, 0.1f, 6.0f}), floor);
  s.addObject(cube, trs({0.0f, 3.1f, 0.0f}, {4.0f, 0.1f, 4.0f}), wall);
  s.addObject(cube, trs({-4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  // Parede do fundo com uma janela (abertura de 2 x 1,6 m fechada por vidro claro).
  s.addObject(cube, trs({-2.5f, 1.5f, -4.1f}, {1.5f, 1.6f, 0.1f}), wall);
  s.addObject(cube, trs({2.5f, 1.5f, -4.1f}, {1.5f, 1.6f, 0.1f}), wall);
  s.addObject(cube, trs({0.0f, 0.35f, -4.1f}, {1.0f, 0.45f, 0.1f}), wall);
  s.addObject(cube, trs({0.0f, 2.75f, -4.1f}, {1.0f, 0.35f, 0.1f}), wall);
  s.addObject(cube, trs({0.0f, 1.6f, -4.1f}, {1.0f, 0.8f, 0.01f}), clear);
  s.addObject(cube, trs({4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  s.addObject(cube, trs({-1.6f, 1.0f, -1.5f}, {0.6f, 1.0f, 0.02f}), tinted);   // painel colorido
  s.addObject(cube, trs({1.8f, 1.0f, -1.0f}, {0.5f, 1.0f, 0.02f}), frosted);   // vidro fosco
  s.addObject(cube, trs({1.8f, 1.0f, -0.9f}, {0.5f, 1.0f, 0.02f}), frosted);   // segunda lâmina (janela dupla)
  s.addObject(cube, trs({0.6f, 0.4f, -2.6f}, glm::vec3(0.4f)), mat({0.8f, 0.2f, 0.1f}, 0.5f));
  s.addObject(cube, trs({-1.6f, 0.5f, -2.6f}, glm::vec3(0.5f)), mat({0.9f, 0.8f, 0.1f}, 0.5f));  // atrás do colorido
  s.addObject(cube, trs({1.9f, 0.5f, -2.2f}, glm::vec3(0.5f)), mat({0.9f, 0.8f, 0.1f}, 0.5f));   // atrás do fosco
  // Lanterna: globo de vidro com uma lâmpada emissiva dentro (luz de malha) sobre um pedestal.
  s.addObject(cube, trs({-0.4f, 0.3f, 0.6f}, {0.1f, 0.3f, 0.1f}), wall);
  s.addObject(sphere, trs({-0.4f, 0.85f, 0.6f}, glm::vec3(0.25f)), clear);
  s.addObject(sphere, trs({-0.4f, 0.85f, 0.6f}, glm::vec3(0.05f)), mat(glm::vec3(1.0f), 0.5f, 0.0f, {60000.0f, 45000.0f, 25000.0f}));
  // Bola de vidro maciço (KHR_materials_volume): refrata (a imagem atrás aparece invertida) e esverdeia por absorção.
  {
    const uint32_t solid = mat(glm::vec3(1.0f), 0.0f, 1.0f);
    s.materials[solid].thickness = 1.0f;
    s.materials[solid].attenuationColor = glm::vec3(0.75f, 0.95f, 0.85f);
    s.materials[solid].attenuationDistance = 0.3f;
    s.addObject(sphere, trs({0.9f, 0.45f, 0.4f}, glm::vec3(0.45f)), solid);
  }
  s.sunDirection = sunFrom(25.0f, 10.0f);  // entra pela janela do fundo
  s.sunIlluminance = 100000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 1.5f, 3.6f);
  camera.lookAt(glm::vec3(0.0f, 1.1f, -3.0f));
  return true;
}

// Teste da pintura de carro: esferas com e sem verniz (KHR_materials_clearcoat) ao sol, sobre um piso listrado que o
// verniz reflete.
bool buildPaintTest(Scene& s, Camera& camera) {
  uint32_t cube = s.addMesh(makeCube());
  uint32_t sphere = s.addMesh(makeSphere(96, 48));
  auto paint = [&](glm::vec3 color, float metallic, float rough, float coat, float coatRough) {
    Material m;
    m.baseColor = glm::vec4(color, 1.0f);
    m.metallic = metallic;
    m.roughness = rough;
    if (coat > 0.0f) {
      m.flags |= kMaterialClearcoat;
      m.clearcoat = coat;
      m.clearcoatRoughness = coatRough;
    }
    return s.addMaterial(m);
  };
  const uint32_t light = paint(glm::vec3(0.6f), 0.0f, 0.8f, 0.0f, 0.0f), dark = paint(glm::vec3(0.08f), 0.0f, 0.8f, 0.0f, 0.0f);
  for (int i = -6; i <= 6; ++i)  // piso listrado
    s.addObject(cube, trs({float(i) * 1.0f, -0.05f, 0.0f}, {0.5f, 0.05f, 6.0f}), (i & 1) ? dark : light);
  const uint32_t mats[] = {paint({0.55f, 0.02f, 0.02f}, 0.3f, 0.45f, 0.0f, 0.0f),   // vermelho sem verniz
                           paint({0.55f, 0.02f, 0.02f}, 0.3f, 0.45f, 1.0f, 0.03f),  // vermelho com verniz
                           paint({0.02f, 0.02f, 0.02f}, 0.0f, 0.5f, 1.0f, 0.02f),   // preto com verniz
                           paint({0.1f, 0.25f, 0.7f}, 0.8f, 0.35f, 1.0f, 0.15f)};   // azul metálico, verniz acetinado
  s.materials[mats[3]].flakes = 0.6f;  // flocos metálicos no azul
  s.materials[mats[3]].flakeScale = 800.0f;
  for (int i = 0; i < 4; ++i) s.addObject(sphere, trs({-2.4f + 1.6f * float(i), 0.6f, 0.0f}, glm::vec3(0.6f)), mats[i]);
  s.sunDirection = sunFrom(35.0f, 150.0f);
  s.sunIlluminance = 100000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 1.4f, 4.2f);
  camera.lookAt(glm::vec3(0.0f, 0.5f, 0.0f));
  return true;
}

// Variante simples da prova das luzes de malha: só o painel do teto e um globo grande e fraco (sem emissores minúsculos
// e muito fortes, que o caminho sem amostragem de luz não converge).
// variant: 0 = painel e globo, 1 = só o painel, 2 = só o globo.
bool buildMeshLightSimple(Scene& s, Camera& camera, int variant) {
  uint32_t cube = s.addMesh(makeCube());
  uint32_t sphere = s.addMesh(makeSphere(32, 16));
  auto mat = [&](glm::vec3 albedo, float rough, glm::vec3 emissive = glm::vec3(0.0f)) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.emissive = emissive;
    return s.addMaterial(m);
  };
  const uint32_t wall = mat(glm::vec3(0.7f), 0.9f), red = mat(glm::vec3(0.7f, 0.08f, 0.06f), 0.8f);
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {4.0f, 0.1f, 4.0f}), wall);
  s.addObject(cube, trs({0.0f, 3.1f, 0.0f}, {4.0f, 0.1f, 4.0f}), wall);
  s.addObject(cube, trs({0.0f, 1.5f, -4.1f}, {4.0f, 1.6f, 0.1f}), red);
  s.addObject(cube, trs({0.0f, 1.5f, 4.1f}, {4.0f, 1.6f, 0.1f}), wall);
  s.addObject(cube, trs({-4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  s.addObject(cube, trs({4.1f, 1.5f, 0.0f}, {0.1f, 1.6f, 4.0f}), wall);
  if (variant != 2 && variant != 4)
    s.addObject(cube, trs({0.0f, 2.95f, -1.0f}, {0.8f, 0.05f, 0.4f}), mat(glm::vec3(1.0f), 0.5f, glm::vec3(300.0f)));
  if (variant == 4)  // painel solto no ar (sem encostar no teto)
    s.addObject(cube, trs({0.0f, 2.3f, -1.0f}, {0.8f, 0.05f, 0.4f}), mat(glm::vec3(1.0f), 0.5f, glm::vec3(300.0f)));
  if (variant == 0 || variant == 2)
    s.addObject(sphere, trs({-1.8f, 0.6f, 0.0f}, glm::vec3(0.6f)), mat(glm::vec3(1.0f), 0.5f, {400.0f, 200.0f, 80.0f}));
  if (variant == 3) {  // o mesmo globo como luz esférica analítica (I = L π r²), com a esfera visível sem sombra
    const uint32_t globe = mat(glm::vec3(1.0f), 0.5f, {400.0f, 200.0f, 80.0f});
    s.materials[globe].flags |= kMaterialLightGeometry | kMaterialEmissionSampled;
    s.addObject(sphere, trs({-1.8f, 0.6f, 0.0f}, glm::vec3(0.6f)), globe);
    s.addLight({{-1.8f, 0.6f, 0.0f}, 0.6f, glm::vec3(400.0f, 200.0f, 80.0f) * glm::pi<float>() * 0.36f, 1e30f});
  }
  s.sunIlluminance = 0.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 1.6f, 3.6f);
  camera.lookAt(glm::vec3(0.0f, 1.0f, -1.5f));
  return true;
}

// Mar aberto: oceano por FFT com um píer e boias, sol e nuvens.
bool buildOcean(Scene& s, Camera& camera, const DemoOptions& opt) {
  Material water;
  water.flags = kMaterialWater;
  s.addObject(s.addMesh(makeRadialGrid(220, 256, 0.6f, 6000.0f)), glm::mat4(1.0f), s.addMaterial(water));
  s.ocean = true;
  uint32_t cube = s.addMesh(makeCube()), sphere = s.addMesh(makeSphere(48, 24));
  auto mat = [&](glm::vec3 albedo, float rough) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    return s.addMaterial(m);
  };
  const uint32_t wood = mat({0.32f, 0.22f, 0.14f}, 0.8f), red = mat({0.7f, 0.06f, 0.04f}, 0.4f), yellow = mat({0.8f, 0.6f, 0.05f}, 0.4f);
  s.addObject(cube, trs({0.0f, 1.2f, 10.0f}, {2.0f, 0.15f, 12.0f}), wood);  // píer
  for (int i = 0; i < 6; ++i)
    for (float x : {-1.8f, 1.8f}) s.addObject(cube, trs({x, -2.0f, -1.0f + 4.0f * float(i)}, {0.15f, 3.2f, 0.15f}), wood);
  for (int i = 0; i < 5; ++i)
    s.addObject(sphere, trs({-12.0f + 6.0f * float(i), 0.2f, -25.0f - 7.0f * float(i % 2)}, glm::vec3(0.6f)), i % 2 ? red : yellow);
  s.sunDirection = sunFrom(opt.night ? -10.0f : 22.0f, 200.0f);
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.cloudCoverage = 0.35f;
  s.fogDensity = 0.00005f;
  camera.position = glm::vec3(0.0f, 3.2f, 20.0f);
  camera.lookAt(glm::vec3(0.0f, 1.5f, -40.0f));
  return true;
}

// Teste da pele: duas esferas do tamanho de uma cabeça com tom de pele sob luz lateral — sem e com espalhamento
// subsuperficial (o terminador fica macio e avermelhado na da direita).
bool buildSkinTest(Scene& s, Camera& camera, bool backlit) {
  uint32_t sphere = s.addMesh(makeSphere(128, 64)), cube = s.addMesh(makeCube());
  Material floor;
  floor.baseColor = glm::vec4(glm::vec3(0.3f), 1.0f);
  floor.roughness = 0.8f;
  s.addObject(cube, trs({0.0f, -0.05f, 0.0f}, {3.0f, 0.05f, 3.0f}), s.addMaterial(floor));
  Material skin;
  skin.baseColor = glm::vec4(0.80f, 0.55f, 0.45f, 1.0f);
  skin.roughness = 0.45f;
  skin.specular = 0.7f;  // f0 ≈ 2,8%: pele
  const uint32_t plain = s.addMaterial(skin);
  skin.flags |= kMaterialSubsurface;
  skin.subsurfaceRadius = glm::vec3(0.0040f, 0.0015f, 0.0007f);
  const uint32_t sss = s.addMaterial(skin);
  s.addObject(sphere, trs({-0.14f, 0.12f, 0.0f}, glm::vec3(0.1f)), plain);
  s.addObject(sphere, trs({0.14f, 0.12f, 0.0f}, glm::vec3(0.1f)), sss);
  // "Orelhas": placas de 4 mm contra a luz (sem e com SSS) — a com SSS acende avermelhada por transmissão.
  s.addObject(cube, trs({-0.1f, 0.33f, -0.15f}, {0.05f, 0.06f, 0.002f}), plain);
  s.addObject(cube, trs({0.1f, 0.33f, -0.15f}, {0.05f, 0.06f, 0.002f}), sss);
  s.sunDirection = sunFrom(20.0f, 95.0f);  // luz de lado: o terminador corta as esferas ao meio
  if (backlit) s.sunDirection = sunFrom(12.0f, 0.0f);  // sol atrás das placas, de frente para a câmera
  s.sunIlluminance = 80000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, backlit ? 0.3f : 0.15f, 0.65f);
  camera.lookAt(glm::vec3(0.0f, backlit ? 0.3f : 0.12f, 0.0f));
  camera.nearPlane = 0.01f;
  return true;
}

// Efeitos (partículas): fogueira, explosão que se repete a cada 5 s e faíscas, num pátio com caixas — à noite.
bool buildVfx(Scene& s, Camera& camera, const DemoOptions& opt) {
  uint32_t cube = s.addMesh(makeCube());
  auto mat = [&](glm::vec3 albedo, float rough) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    return s.addMaterial(m);
  };
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {40.0f, 0.1f, 40.0f}), mat(glm::vec3(0.3f, 0.28f, 0.25f), 0.9f));
  const uint32_t crate = mat({0.4f, 0.28f, 0.16f}, 0.8f), concrete = mat(glm::vec3(0.5f), 0.85f);
  for (int i = 0; i < 6; ++i) s.addObject(cube, trs({-8.0f + 3.2f * float(i), 0.6f, -10.0f}, glm::vec3(0.6f)), crate);
  s.addObject(cube, trs({0.0f, 3.0f, -16.0f}, {14.0f, 3.0f, 0.4f}), concrete);
  s.addEmitter(kParticleFire, {-5.0f, 0.0f, -4.0f}, 0.35f, 1.0f, 900);
  s.addEmitter(kParticleSmoke, {-5.0f, 0.0f, -4.0f}, 0.3f, 1.0f, 500);
  s.addEmitter(kParticleSparks, {-5.0f, 0.0f, -4.0f}, 0.2f, 1.0f, 150);
  s.addEmitter(kParticleExplosion, {4.0f, 0.5f, -8.0f}, 0.0f, 1.0f, 2500);
  s.sunIlluminance = opt.night ? 0.0f : 100000.0f;
  s.sunDirection = sunFrom(40.0f, 120.0f);
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 2.0f, 8.0f);
  camera.lookAt(glm::vec3(0.0f, 2.0f, -8.0f));
  return true;
}

// Multidão: 300 pessoas (CesiumMan) andando em círculos numa praça, com animação assada (32 poses do ciclo): sem
// skinning por pessoa; sombras e reflexos pela BLAS da pose de cada uma.
bool buildCrowd(Scene& s, Camera& camera, const DemoOptions& opt) {
  uint32_t cube = s.addMesh(makeCube());
  Material ground;
  ground.baseColor = glm::vec4(0.45f, 0.43f, 0.40f, 1.0f);
  ground.roughness = 0.8f;
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {60.0f, 0.1f, 60.0f}), s.addMaterial(ground));
  float foot = 0.0f;
  const int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", foot);
  if (man < 0) return false;
  s.models[size_t(man)].root = glm::translate(glm::mat4(1.0f), {0.0f, -100.0f, 0.0f});  // o original fica escondido
  const uint32_t clip = s.bakeCrowdClip(man, 32);
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  const uint32_t count = opt.lightCount ? opt.lightCount : 300;  // --lights N reaproveitado como tamanho da multidão
  for (uint32_t i = 0; i < count; ++i) {
    const float radius = 4.0f + 26.0f * std::sqrt(u(rng)), phase = glm::two_pi<float>() * u(rng);
    const float speed = (1.1f + 0.5f * u(rng)) * (u(rng) < 0.5f ? 1.0f : -1.0f);  // m/s, metade em cada sentido
    const float scale = 0.9f + 0.2f * u(rng);
    const uint32_t agent = s.addCrowdAgent(clip, glm::mat4(1.0f), 3.0f * u(rng));
    s.addAnimator([=](Scene& sc, float t, float) {
      const float th = phase + speed / radius * t;
      const glm::vec3 p(radius * std::cos(th), foot * scale, radius * std::sin(th));
      const glm::vec3 v = glm::vec3(-std::sin(th), 0.0f, std::cos(th)) * (speed > 0.0f ? 1.0f : -1.0f);
      glm::mat4 m = glm::translate(glm::mat4(1.0f), p);
      m = glm::rotate(m, std::atan2(v.x, v.z) + kCesiumManFacing, glm::vec3(0, 1, 0));
      sc.setTransform(agent, glm::scale(m, glm::vec3(scale)));
    });
  }
  s.sunDirection = sunFrom(opt.night ? -10.0f : 40.0f, 140.0f);
  s.sunIlluminance = opt.night ? 0.0f : 110000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 6.0f, 38.0f);
  camera.lookAt(glm::vec3(0.0f, 0.5f, 0.0f));
  return true;
}

bool buildDemoSceneDefaults(Scene& scene, Camera& camera, const DemoOptions& opt);

bool buildDemoScene(Scene& scene, Camera& camera, const DemoOptions& opt) {
  if (!buildDemoSceneDefaults(scene, camera, opt)) return false;
  if (opt.clouds >= 0.0f) scene.cloudCoverage = std::min(opt.clouds, 1.0f);
  if (opt.fog >= 0.0f) scene.fogDensity = opt.fog;
  if (opt.rain > 0.0f) {
    scene.wetness = std::min(opt.rain * 1.5f, 1.0f);  // molha antes de empoçar
    scene.puddles = std::min(opt.rain, 1.0f);
    scene.rain = std::min(opt.rain, 1.0f);
    if (opt.clouds < 0.0f && scene.sunIlluminance > 0.0f)  // chuva vem com céu fechado
      scene.cloudCoverage = std::max(scene.cloudCoverage, 0.5f + 0.4f * scene.rain);
    scene.addEmitter(kParticleRain, glm::vec3(0.0f), 0.0f, scene.rain, uint32_t(40000.0f * scene.rain));  // pingos
    scene.addEmitter(kParticleSplash, glm::vec3(0.0f), 0.0f, scene.rain, uint32_t(4000.0f * scene.rain));  // respingos
  }
  return true;
}

bool buildDemoSceneDefaults(Scene& scene, Camera& camera, const DemoOptions& opt) {
  auto endsWith = [&](const char* ext) {
    size_t n = std::strlen(ext);
    return opt.name.size() > n && opt.name.compare(opt.name.size() - n, n, ext) == 0;
  };
  if (endsWith(".gltf") || endsWith(".glb")) return buildFromFile(scene, camera, opt);
  if (opt.name == "outdoor") return buildOutdoor(scene, camera, opt);
  if (opt.name == "furnace") return buildFurnace(scene, camera);
  if (opt.name == "shadows") return buildShadowTest(scene, camera, glm::vec3(0.0f));
  if (opt.name == "shadows-far") return buildShadowTest(scene, camera, glm::vec3(4000.0f, 0.0f, -3000.0f));
  if (opt.name == "reflections") return buildReflectionTest(scene, camera);
  if (opt.name == "city") return buildCity(scene, camera, opt);
  if (opt.name == "mesh-lights") return buildMeshLightTest(scene, camera);
  if (opt.name == "mesh-lights-simple") return buildMeshLightSimple(scene, camera, 0);
  if (opt.name == "mesh-lights-panel") return buildMeshLightSimple(scene, camera, 1);
  if (opt.name == "mesh-lights-ball") return buildMeshLightSimple(scene, camera, 2);
  if (opt.name == "mesh-lights-ball-analytic") return buildMeshLightSimple(scene, camera, 3);
  if (opt.name == "mesh-lights-panel-free") return buildMeshLightSimple(scene, camera, 4);
  if (opt.name == "glass") return buildGlassTest(scene, camera);
  if (opt.name == "ocean") return buildOcean(scene, camera, opt);
  if (opt.name == "paint") return buildPaintTest(scene, camera);
  if (opt.name == "skin") return buildSkinTest(scene, camera, false);
  if (opt.name == "skin-backlit") return buildSkinTest(scene, camera, true);
  if (opt.name == "vfx") return buildVfx(scene, camera, opt);
  if (opt.name == "crowd") return buildCrowd(scene, camera, opt);
  return buildSponza(scene, camera, opt);
}

}  // namespace scene
