#include "scene/demo_scene.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <random>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace scene {

void addTerrainLayers(Scene& s);  // camadas texturizadas do terreno (definida mais abaixo)

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
  addTerrainLayers(s);
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
// Imagens geradas na CPU para decals: "pichação" (palavra em blocos grossos com contorno e respingos) e "cartaz"
// (retângulo com faixas e um círculo). RGBA com alfa recortado; 256 x 128.
TextureData makeGraffiti(uint32_t seed) {
  const uint32_t W = 256, H = 128;
  TextureData t;
  t.width = W;
  t.height = H;
  t.srgb = true;
  t.name = "pichacao";
  t.rgba.assign(size_t(W) * H * 4, 0);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  const glm::vec3 fill = hue(u(rng)) * 0.9f + 0.05f, outline(0.05f), shade = fill * 0.55f;
  // Letras: 5 blocos com "furos", tortos e com alturas diferentes (estilo bloco de pichação).
  auto inLetters = [&](float x, float y, float grow) {
    for (int k = 0; k < 5; ++k) {
      const float cx = 30.0f + float(k) * 48.0f, cy = 64.0f + 10.0f * std::sin(float(k) * 1.7f + float(seed));
      const float w = 18.0f + grow, h = 34.0f + grow + 6.0f * std::sin(float(k) * 2.3f);
      const float dx = (x - cx) - 0.25f * (y - cy), dy = y - cy;
      if (std::abs(dx) < w && std::abs(dy) < h && !(std::abs(dx) < 6.0f - grow && std::abs(dy) < 12.0f - grow)) return true;
    }
    return false;
  };
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      glm::vec4 c(0.0f);
      if (inLetters(float(x), float(y), 4.0f)) c = glm::vec4(outline, 1.0f);           // contorno
      if (inLetters(float(x), float(y), 0.0f)) c = glm::vec4(y > 64 ? shade : fill, 1.0f);  // preenchimento com sombra
      if (c.a == 0.0f && u(rng) < 0.004f) c = glm::vec4(fill, 1.0f);                  // respingos de spray
      uint8_t* p = &t.rgba[(size_t(y) * W + x) * 4];
      for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::pow(glm::clamp(c[k], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
      p[3] = uint8_t(c.a * 255.0f);
    }
  return t;
}
TextureData makePoster(uint32_t seed) {
  const uint32_t W = 128, H = 192;
  TextureData t;
  t.width = W;
  t.height = H;
  t.srgb = true;
  t.name = "cartaz";
  t.rgba.assign(size_t(W) * H * 4, 255);
  const glm::vec3 a = hue(float(seed) * 0.37f), b = hue(float(seed) * 0.37f + 0.5f);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < W; ++x) {
      glm::vec3 c = y < 40 ? a : (y > 170 ? glm::vec3(0.1f) : glm::vec3(0.92f, 0.9f, 0.85f));
      const float dx = float(x) - 64.0f, dy = float(y) - 105.0f;
      if (dx * dx + dy * dy < 40.0f * 40.0f) c = b;
      if (y > 172 && y < 186 && (x / 6) % 2 == 0 && x > 16 && x < 112) c = glm::vec3(0.9f);  // "texto"
      uint8_t* p = &t.rgba[(size_t(y) * W + x) * 4];
      for (int k = 0; k < 3; ++k) p[k] = uint8_t(std::pow(glm::clamp(c[k], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f);
      p[3] = 255;
    }
  return t;
}

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

  uint32_t graffiti[3], poster[3];
  for (uint32_t k = 0; k < 3; ++k) {
    graffiti[k] = s.addTexture(makeGraffiti(k + 1));
    poster[k] = s.addTexture(makePoster(k + 3));
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
        // Pichação e cartazes nas fachadas voltadas para a rua leste (a face de +x dos prédios do lote da direita).
        // Muro na beira da calçada (lado da rua leste) com pichação e cartazes.
        const float wallX = cx + 0.5f * block - 0.4f;
        s.addObject(cube, trs({wallX, 1.6f, cz}, {0.15f, 1.45f, 12.0f}), concrete[1]);
        s.addWallDecal(int32_t(graffiti[(bx + bz) % 3]), {wallX + 0.15f, 1.6f, cz + 5.0f}, {1.0f, 0.0f, 0.0f}, 5.0f, 2.5f, 0.7f);
        s.addWallDecal(int32_t(poster[(bx * 3 + bz) % 3]), {wallX + 0.15f, 1.7f, cz - 3.0f}, {1.0f, 0.0f, 0.0f}, 1.2f, 1.8f, 0.5f);
        s.addWallDecal(int32_t(poster[(bx * 3 + bz + 1) % 3]), {wallX + 0.15f, 1.7f, cz - 4.4f}, {1.0f, 0.0f, 0.0f}, 1.2f, 1.8f, 0.5f);
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
  // Nada da cidade se move: tudo pode virar HLOD. Células de 128 m (~1 quarteirão e meio), proxy a partir de 250 m.
  for (Object& o : s.objects) o.hlodCandidate = true;
  s.hlodCellSize = 128.0f;
  s.hlodDistance = 250.0f;
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
  // Fundo de areia a 6 m (com pedras), para a vista subaquática e as cáusticas.
  const uint32_t sand = mat({0.62f, 0.55f, 0.42f}, 0.9f), rock = mat({0.25f, 0.24f, 0.22f}, 0.8f);
  s.addObject(cube, trs({0.0f, -6.5f, 0.0f}, {80.0f, 0.5f, 80.0f}), sand);
  for (int i = 0; i < 8; ++i)
    s.addObject(sphere, trs({-10.0f + 3.0f * float(i), -6.0f, -6.0f - 2.0f * float(i % 3)}, glm::vec3(0.8f, 0.6f, 0.9f)), rock);
  // Boia vermelha dando voltas (3 m/s) metade dentro d'água: empurra a água e deixa anéis e esteira (ripple.comp).
  {
    const uint32_t buoy = s.addObject(sphere, trs({8.0f, 0.0f, -8.0f}, glm::vec3(0.7f)), red);
    s.addAnimator([=](Scene& sc, float t, float) {
      const float a = t * 3.0f / 8.0f;
      sc.setTransform(buoy, trs({8.0f * std::cos(a), sc.seaLevel, -8.0f + 8.0f * std::sin(a)}, glm::vec3(0.7f)));
    });
  }
  s.sunDirection = sunFrom(opt.night ? -10.0f : 22.0f, 200.0f);
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.cloudCoverage = 0.35f;
  s.fogDensity = 0.00005f;
  camera.position = glm::vec3(0.0f, 3.2f, 20.0f);
  camera.lookAt(glm::vec3(0.0f, 1.5f, -40.0f));
  return true;
}

// Texturas das camadas do terreno, geradas na CPU (repetem sem emenda): cor com a altura no alfa e o normal map
// tirado da altura. Grama (fios curtos e touceiras), terra (pedriscos), rocha (placas com rachaduras) e cascalho.
namespace {
uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}
float rnd2(int x, int y, uint32_t seed) { return float(hash2(x, y, seed) & 0xFFFFFF) / 16777216.0f; }
int wrapi(int v, int p) { return ((v % p) + p) % p; }
// Ruído de valor periódico (período p células) e fbm.
float pnoise(float x, float y, int p, uint32_t seed) {
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  const float fx = x - float(ix), fy = y - float(iy), ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
  auto v = [&](int a, int b) { return rnd2(wrapi(a, p), wrapi(b, p), seed); };
  return glm::mix(glm::mix(v(ix, iy), v(ix + 1, iy), ux), glm::mix(v(ix, iy + 1), v(ix + 1, iy + 1), ux), uy);
}
float pfbm(float u, float v, int base, int octaves, uint32_t seed) {
  float s = 0.0f, a = 0.5f, n = 0.0f;
  for (int o = 0; o < octaves; ++o) {
    const int p = base << o;
    s += a * pnoise(u * float(p), v * float(p), p, seed + uint32_t(o));
    n += a;
    a *= 0.5f;
  }
  return s / n;
}
// Worley periódico: distância ao ponto mais próximo (f1), ao segundo (f2) e o sorteio da célula mais próxima.
void pworley(float u, float v, int p, uint32_t seed, float& f1, float& f2, float& cell) {
  const float x = u * float(p), y = v * float(p);
  const int ix = int(std::floor(x)), iy = int(std::floor(y));
  f1 = f2 = 1e9f;
  cell = 0.0f;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      const int cx = ix + dx, cy = iy + dy;
      const float px = float(cx) + rnd2(wrapi(cx, p), wrapi(cy, p), seed), py = float(cy) + rnd2(wrapi(cx, p), wrapi(cy, p), seed + 7u);
      const float d = std::hypot(px - x, py - y);
      if (d < f1) {
        f2 = f1;
        f1 = d;
        cell = rnd2(wrapi(cx, p), wrapi(cy, p), seed + 13u);
      } else if (d < f2) {
        f2 = d;
      }
    }
}
}  // namespace

void makeTerrainLayer(int kind, uint32_t size, TextureData& albedo, TextureData& normal) {
  std::vector<float> h(size_t(size) * size);
  std::vector<glm::vec3> c(h.size());
  for (uint32_t y = 0; y < size; ++y)
    for (uint32_t x = 0; x < size; ++x) {
      const float u = (float(x) + 0.5f) / float(size), v = (float(y) + 0.5f) / float(size);
      float height = 0.5f;
      glm::vec3 col(0.5f);
      if (kind == 0) {  // grama: touceiras grossas + fios finos (ruído esticado em direções variadas)
        const float clump = pfbm(u, v, 8, 4, 11u), blades = pnoise(u * 256.0f, v * 64.0f, 256, 3u) * 0.5f +
                                                        pnoise(u * 64.0f, v * 256.0f, 64, 5u) * 0.5f;
        height = 0.35f * clump + 0.65f * blades;
        col = glm::mix(glm::vec3(0.05f, 0.09f, 0.02f), glm::vec3(0.16f, 0.20f, 0.05f), clump) * (0.55f + 0.7f * blades);
        col = glm::mix(col, glm::vec3(0.20f, 0.17f, 0.08f), glm::smoothstep(0.62f, 0.8f, pfbm(u, v, 4, 3, 21u)) * 0.6f);  // seca
      } else if (kind == 1) {  // terra: base fina + pedriscos
        float f1, f2, cell;
        pworley(u, v, 48, 31u, f1, f2, cell);
        const float pebble = glm::smoothstep(0.42f, 0.18f, f1) * (cell > 0.55f ? 1.0f : 0.0f), grain = pfbm(u, v, 32, 4, 33u);
        height = 0.4f * grain + 0.6f * pebble;
        col = glm::vec3(0.20f, 0.14f, 0.09f) * (0.7f + 0.5f * grain);
        col = glm::mix(col, glm::vec3(0.30f, 0.27f, 0.23f) * (0.8f + 0.4f * cell), pebble);
      } else if (kind == 2) {  // rocha: placas (Worley) com rachaduras nas bordas, manchas grandes
        float f1, f2, cell;
        pworley(u, v, 6, 41u, f1, f2, cell);
        const float crack = glm::smoothstep(0.0f, 0.06f, f2 - f1), detail = pfbm(u, v, 16, 5, 43u), blot = pfbm(u, v, 3, 3, 47u);
        height = (0.55f + 0.25f * cell + 0.3f * detail) * (0.35f + 0.65f * crack);
        col = glm::mix(glm::vec3(0.22f, 0.21f, 0.19f), glm::vec3(0.36f, 0.34f, 0.30f), detail) * (0.75f + 0.4f * blot);
        col *= 0.45f + 0.55f * crack;
      } else {  // cascalho: pedras pequenas em várias cores
        float f1, f2, cell;
        pworley(u, v, 40, 51u, f1, f2, cell);
        const float stone = glm::smoothstep(0.0f, 0.25f, f2 - f1);
        height = stone * (0.7f + 0.3f * cell);
        col = glm::mix(glm::vec3(0.24f, 0.22f, 0.19f), glm::vec3(0.40f, 0.37f, 0.33f), cell) * (0.35f + 0.65f * stone);
      }
      h[size_t(y) * size + x] = glm::clamp(height, 0.0f, 1.0f);
      c[size_t(y) * size + x] = col;
    }
  albedo = {};
  albedo.width = albedo.height = size;
  albedo.srgb = true;
  albedo.name = "terreno_cor";
  albedo.rgba.resize(h.size() * 4);
  normal = {};
  normal.width = normal.height = size;
  normal.srgb = false;
  normal.normalMap = true;
  normal.name = "terreno_normal";
  normal.rgba.resize(h.size() * 4);
  const float bump = float(size) / 128.0f * (kind == 2 ? 1.6f : 1.0f);  // relevo: rocha mais funda
  for (uint32_t y = 0; y < size; ++y)
    for (uint32_t x = 0; x < size; ++x) {
      const size_t i = size_t(y) * size + x;
      for (int k = 0; k < 3; ++k) albedo.rgba[i * 4 + k] = uint8_t(std::pow(glm::clamp(c[i][k], 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f + 0.5f);
      albedo.rgba[i * 4 + 3] = uint8_t(h[i] * 255.0f + 0.5f);
      auto H = [&](int xx, int yy) { return h[size_t(wrapi(yy, int(size))) * size + size_t(wrapi(xx, int(size)))]; };
      const float dx = (H(int(x) + 1, int(y)) - H(int(x) - 1, int(y))) * 0.5f * bump;
      const float dy = (H(int(x), int(y) + 1) - H(int(x), int(y) - 1)) * 0.5f * bump;
      const glm::vec3 n = glm::normalize(glm::vec3(-dx, -dy, 1.0f));
      for (int k = 0; k < 3; ++k) normal.rgba[i * 4 + k] = uint8_t((n[k] * 0.5f + 0.5f) * 255.0f + 0.5f);
      normal.rgba[i * 4 + 3] = 255;
    }
}

// Liga as camadas texturizadas ao terreno da cena (texturas de 512², repetidas pelos metros de cada camada).
void addTerrainLayers(Scene& s) {
  // Em verificação (custo de carga e de sombreamento ainda sendo medidos): só com LEGACY_TERRAIN_LAYERS=1.
  if (!std::getenv("LEGACY_TERRAIN_LAYERS")) return;
  for (int k = 0; k < 4; ++k) {
    TextureData a, n;
    makeTerrainLayer(k, 512, a, n);
    s.terrainLayers.albedo[k] = int(s.addTexture(std::move(a)));
    s.terrainLayers.normal[k] = int(s.addTexture(std::move(n)));
  }
}

// Pele com um scan real: a cabeça do Lee Perry-Smith (Infinite-Realities, CC BY 3.0; a do exemplo do three.js), com
// a textura de cor e o normal map do scan, pele com espalhamento subsuperficial, luz de lado (ou por trás).
bool buildScanHead(Scene& s, Camera& camera, const DemoOptions& opt, bool backlit) {
  const std::string dir = kAssets + "/LeePerrySmith/";
  const size_t first = s.objects.size();
  if (!loadGltf(s, dir + "LeePerrySmith.glb", glm::mat4(1.0f))) return false;
  glm::vec3 lo, hi;
  s.bounds(lo, hi);
  const float scale = 0.23f / std::max(hi.y - lo.y, 1e-6f);  // altura real de uma cabeça
  const glm::vec3 center = 0.5f * (lo + hi);
  const glm::mat4 fit = glm::translate(glm::mat4(1.0f), {0.0f, 0.35f, 0.0f}) * glm::scale(glm::mat4(1.0f), glm::vec3(scale)) *
                        glm::translate(glm::mat4(1.0f), -center);
  TextureData col, nrm;
  if (!loadImageFile(dir + "Map-COL.jpg", true, false, col) || !loadImageFile(dir + "Infinite-Level_02_Tangent_SmoothUV.jpg", false, true, nrm))
    return false;
  Material skin;
  skin.baseColorTex = int32_t(s.addTexture(std::move(col)));
  skin.normalTex = int32_t(s.addTexture(std::move(nrm)));
  skin.roughness = 0.45f;
  skin.specular = 0.7f;
  skin.flags = kMaterialSubsurface;
  skin.subsurfaceRadius = glm::vec3(0.0040f, 0.0015f, 0.0007f);
  const uint32_t skinMat = s.addMaterial(skin);
  for (size_t i = first; i < s.objects.size(); ++i) {
    s.objects[i].transform = s.objects[i].prevTransform = fit * s.objects[i].transform;
    s.objects[i].material = skinMat;
  }
  Material floorM;
  floorM.baseColor = glm::vec4(glm::vec3(0.3f), 1.0f);
  floorM.roughness = 0.8f;
  s.addObject(s.addMesh(makeCube()), trs({0.0f, -0.05f, 0.0f}, {2.0f, 0.05f, 2.0f}), s.addMaterial(floorM));
  s.sunDirection = backlit ? sunFrom(15.0f, 180.0f) : sunFrom(25.0f, 60.0f);
  s.sunIlluminance = 90000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(0.0f, 0.38f, 0.55f);
  camera.lookAt(glm::vec3(0.0f, 0.36f, 0.0f));
  camera.nearPlane = 0.01f;
  return true;
}

// Rio (água corrente, kMaterialRiver): vale escavado num terreno ondulado, a fita d'água seguindo uma curva com a
// tangente na direção da correnteza, pedras no leito e grama nas margens.
namespace {
float riverCenter(float x) { return 18.0f * std::sin(x * 0.012f) + 6.0f * std::sin(x * 0.041f + 1.3f); }
constexpr float kRiverHalfWidth = 9.0f, kRiverLevel = -1.4f;
float riverTerrain(float x, float z) {
  // Morros de até ±0,8 m: a margem (a 15 m do eixo) nunca desce abaixo da água (−1,4), o leito fica a ~1,5 m.
  const float hills = 0.6f * std::sin(x * 0.021f) * std::cos(z * 0.017f) + 0.2f * std::sin(x * 0.07f + z * 0.05f);
  const float dz = std::abs(z - riverCenter(x));
  // Leito a 2,6 m abaixo da margem, barranco suave de 6 m.
  const float carve = 2.6f * (1.0f - glm::smoothstep(kRiverHalfWidth - 2.0f, kRiverHalfWidth + 6.0f, dz));
  return hills + 0.6f - carve - 0.9f;
}
}  // namespace

bool buildRiver(Scene& s, Camera& camera, const DemoOptions& opt) {
  Material ground;
  ground.flags = kMaterialTerrain;
  addTerrainLayers(s);
  s.addObject(s.addMesh(makeTerrain(800.0f, 400, riverTerrain)), glm::mat4(1.0f), s.addMaterial(ground));
  // Fita do rio: um par de vértices a cada 2 m ao longo da curva, tangente = direção da correnteza (+x).
  MeshData river;
  // Malha fina (1 m ao longo, 16 faixas na largura): as ondulações dos objetos deslocam os vértices (gbuffer.vert).
  constexpr int kAcross = 16;
  for (int i = 0; i <= 400; ++i) {
    const float x = -200.0f + 1.0f * float(i), zc = riverCenter(x);
    const glm::vec3 dir = glm::normalize(glm::vec3(2.0f, 0.0f, riverCenter(x + 1.0f) - riverCenter(x - 1.0f)));
    // Mais larga que o canal: a borda da fita fica sempre enterrada no barranco.
    const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0, 1, 0), dir)) * (kRiverHalfWidth + 8.0f);
    for (int k = 0; k <= kAcross; ++k) {
      const float f = float(k) / float(kAcross) * 2.0f - 1.0f;
      Vertex v;
      v.position = glm::vec3(x, kRiverLevel, zc) + side * f;
      v.normal = glm::vec3(0, 1, 0);
      v.uv = glm::vec2(0.5f * f + 0.5f, x * 0.05f);
      v.tangent = glm::vec4(dir, 1.0f);
      river.vertices.push_back(v);
    }
    if (i > 0)
      for (int k = 0; k < kAcross; ++k) {
        const uint32_t a = uint32_t((i - 1) * (kAcross + 1) + k), b = a + kAcross + 1;
        river.indices.insert(river.indices.end(), {a, b, a + 1, a + 1, b, b + 1});  // frente para cima
      }
  }
  Material water;
  water.flags = kMaterialRiver;
  water.baseColor = glm::vec4(0.018f, 0.03f, 0.024f, 1.0f);  // rio verde-escuro, com sedimento
  water.flowSpeed = 1.8f;
  s.addObject(s.addMesh(std::move(river)), glm::mat4(1.0f), s.addMaterial(water));
  // Pedras no leito (algumas furando a superfície) e grama nas margens.
  Material rockM;
  rockM.baseColor = glm::vec4(0.22f, 0.21f, 0.19f, 1.0f);
  rockM.roughness = 0.85f;
  const uint32_t rock = s.addMaterial(rockM), sphere = s.addMesh(makeSphere(32, 16));
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  for (int i = 0; i < 40; ++i) {
    const float x = -60.0f + 140.0f * u(rng), z = riverCenter(x) + (u(rng) * 2.0f - 1.0f) * kRiverHalfWidth;
    const float r = 0.4f + 1.1f * u(rng);
    s.addObject(sphere, trs({x, riverTerrain(x, z) + 0.3f * r, z}, {r * 1.3f, r * 0.8f, r}), rock);
  }
  Material grass;
  grass.baseColor = glm::vec4(0.16f, 0.24f, 0.06f, 1.0f);
  grass.roughness = 0.6f;
  grass.flags = kMaterialDoubleSided | kMaterialFoliage;
  const uint32_t grassMat = s.addMaterial(grass);
  uint32_t clumps[4];
  for (uint32_t v = 0; v < 4; ++v) clumps[v] = s.addMesh(makeGrassClump(14, 0.45f, v + 1));
  for (int i = 0; i < 5000; ++i) {
    const float x = -40.0f + 100.0f * u(rng), z = riverCenter(x) + (u(rng) < 0.5f ? -1.0f : 1.0f) * (kRiverHalfWidth + 7.0f + 18.0f * u(rng));
    glm::mat4 t = glm::translate(glm::mat4(1.0f), {x, riverTerrain(x, z) - 0.02f, z});
    t = glm::rotate(t, glm::two_pi<float>() * u(rng), glm::vec3(0, 1, 0));
    s.addObject(clumps[i % 4], glm::scale(t, glm::vec3(0.7f + 0.6f * u(rng))), grassMat);
  }
  // Barquinho subindo o rio a 3 m/s (meio afundado): a esteira e as ondas dele vêm das ondulações (ripple.comp).
  {
    Material hullM;
    hullM.baseColor = glm::vec4(0.55f, 0.32f, 0.12f, 1.0f);
    hullM.roughness = 0.6f;
    const uint32_t boat = s.addObject(s.addMesh(makeCube()), glm::mat4(1.0f), s.addMaterial(hullM));
    auto place = [](float t) {
      const float x = -6.0f - std::fmod(t * 3.0f, 50.0f);  // passa pela câmera (x = −30)
      const float yaw = std::atan2(riverCenter(x + 1.0f) - riverCenter(x - 1.0f), 2.0f);
      glm::mat4 m = glm::translate(glm::mat4(1.0f), {x, kRiverLevel, riverCenter(x) - 2.0f});
      return glm::scale(glm::rotate(m, -yaw, glm::vec3(0, 1, 0)), glm::vec3(1.6f, 0.35f, 0.6f));
    };
    s.setTransform(boat, place(0.0f));
    s.addAnimator([=](Scene& sc, float t, float) { sc.setTransform(boat, place(t)); });
  }
  s.sunDirection = sunFrom(opt.night ? -10.0f : 28.0f, 210.0f);
  s.sunIlluminance = opt.night ? 0.0f : 110000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.cloudCoverage = 0.35f;
  const float cx = -30.0f;
  camera.position = glm::vec3(cx, 2.4f, riverCenter(cx) + kRiverHalfWidth + 4.0f);
  camera.lookAt(glm::vec3(cx + 40.0f, -0.5f, riverCenter(cx + 40.0f)));
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
  // Olhos (12 mm de raio) entre as esferas, olhando para a câmera; córnea com reflexo.
  {
    Material eye;
    eye.baseColor = glm::vec4(0.12f, 0.32f, 0.45f, 1.0f);  // íris azul-esverdeada
    eye.flags = kMaterialEye | kMaterialClearcoat;
    eye.clearcoat = 1.0f;
    eye.clearcoatRoughness = 0.02f;
    const uint32_t eyeMat = s.addMaterial(eye);
    for (float x : {-0.02f, 0.02f}) s.addObject(sphere, trs({x, 0.22f, 0.05f}, glm::vec3(0.012f)), eyeMat);
  }
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
  s.addObject(cube, trs({0.0f, -0.1f, 0.0f}, {400.0f, 0.1f, 400.0f}), s.addMaterial(ground));
  float foot = 0.0f;
  const int man = loadCharacter(s, "CesiumMan/CesiumMan.gltf", 1.0f, "", foot);
  if (man < 0) return false;
  s.models[size_t(man)].root = glm::translate(glm::mat4(1.0f), {0.0f, -100.0f, 0.0f});  // o original fica escondido
  const uint32_t clip = s.bakeCrowdClip(man, 32);
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  const uint32_t count = opt.lightCount ? opt.lightCount : 300;  // --lights N reaproveitado como tamanho da multidão
  for (uint32_t i = 0; i < count; ++i) {
    // A praça cresce com a multidão (densidade de 300 pessoas num raio de 30 m).
    const float spread = 26.0f * std::sqrt(std::max(float(count) / 300.0f, 1.0f));
    const float radius = 4.0f + spread * std::sqrt(u(rng)), phase = glm::two_pi<float>() * u(rng);
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

// Cabelo por fios: cabeça (esfera com pele e SSS), olhos e 12 mil fios de 25 cm, castanhos, ao sol de lado.
bool buildHairTest(Scene& s, Camera& camera) {
  uint32_t sphere = s.addMesh(makeSphere(96, 48)), cube = s.addMesh(makeCube());
  Material floor;
  floor.baseColor = glm::vec4(glm::vec3(0.3f), 1.0f);
  floor.roughness = 0.8f;
  s.addObject(cube, trs({0.0f, -0.05f, 0.0f}, {2.0f, 0.05f, 2.0f}), s.addMaterial(floor));
  Material skin;
  skin.baseColor = glm::vec4(0.80f, 0.56f, 0.46f, 1.0f);
  skin.roughness = 0.45f;
  skin.specular = 0.7f;
  skin.flags = kMaterialSubsurface;
  skin.subsurfaceRadius = glm::vec3(0.0040f, 0.0015f, 0.0007f);
  const glm::vec3 head(0.0f, 0.35f, 0.0f);
  const uint32_t first = uint32_t(s.objects.size());  // cabeça, olhos e cabelo giram juntos (abaixo)
  s.addObject(sphere, trs(head, glm::vec3(0.09f, 0.11f, 0.095f)), s.addMaterial(skin));
  Material eye;
  eye.baseColor = glm::vec4(0.25f, 0.16f, 0.08f, 1.0f);  // castanho
  eye.flags = kMaterialEye | kMaterialClearcoat;
  eye.clearcoat = 1.0f;
  eye.clearcoatRoughness = 0.02f;
  const uint32_t eyeMat = s.addMaterial(eye);
  for (float x : {-0.032f, 0.032f}) s.addObject(sphere, trs(head + glm::vec3(x, 0.015f, 0.08f), glm::vec3(0.011f)), eyeMat);
  Material hair;
  hair.baseColor = glm::vec4(0.28f, 0.14f, 0.06f, 1.0f);  // castanho
  hair.roughness = 0.35f;
  hair.flags = kMaterialHair | kMaterialDoubleSided;
  // Fios na esfera de raio 1 escalada para a cabeça (o couro fica 3% para fora da pele).
  glm::mat4 t = glm::scale(glm::translate(glm::mat4(1.0f), head), glm::vec3(0.093f, 0.113f, 0.098f));
  s.addObject(s.addMesh(makeHair(14000, 14, 1.0f, 2.6f, 0.0012f, 7)), t, s.addMaterial(hair));
  // A cabeça vira para os lados e balança um pouco (o cabelo, simulado, atrasa e balança depois: hair_sim.comp).
  {
    std::vector<std::pair<uint32_t, glm::mat4>> parts;
    for (uint32_t i = first; i < s.objects.size(); ++i) parts.push_back({i, s.objects[i].transform});
    s.addAnimator([=](Scene& sc, float time, float) {
      const float yaw = 0.6f * std::sin(time * 2.2f), nod = 0.15f * std::sin(time * 3.1f + 0.7f);
      glm::mat4 r = glm::translate(glm::mat4(1.0f), head);
      r = glm::rotate(r, yaw, glm::vec3(0, 1, 0));
      r = glm::rotate(r, nod, glm::vec3(1, 0, 0));
      r = glm::translate(r, -head);
      for (const auto& [o, base] : parts) sc.setTransform(o, r * base);
    });
  }
  s.sunDirection = sunFrom(30.0f, 60.0f);
  s.sunIlluminance = 90000.0f;
  s.fogDensity = 0.0f;
  camera.position = head + glm::vec3(0.25f, 0.03f, 0.45f);
  camera.lookAt(head);
  camera.nearPlane = 0.01f;
  return true;
}

// Acrescenta uma caixa (centro, meias-medidas) a uma malha, com normais e UVs por face.
void appendBox(MeshData& m, glm::vec3 c, glm::vec3 h) {
  const glm::vec3 n[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (const glm::vec3& nf : n) {
    const glm::vec3 u = std::abs(nf.y) > 0.5f ? glm::vec3(1, 0, 0) : glm::normalize(glm::cross(glm::vec3(0, 1, 0), nf));
    const glm::vec3 v = glm::cross(nf, u);
    const uint32_t base = uint32_t(m.vertices.size());
    for (int k = 0; k < 4; ++k) {
      const float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sv = (k >= 2) ? 1.0f : -1.0f;
      const glm::vec3 p = c + h * (nf + u * su + v * sv);
      m.vertices.push_back({p, nf, {su * 0.5f + 0.5f, sv * 0.5f + 0.5f}, {u, 1.0f}});
    }
    m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
  }
}

// Torre procedural única: andares escalonados (recuos), janelas em relevo em todas as fachadas, caixa d'água.
MeshData makeTower(uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  MeshData m;
  float w = 8.0f + 8.0f * u(rng), d = 8.0f + 8.0f * u(rng), y = 0.0f;
  const int tiers = 2 + int(u(rng) * 3.0f);
  for (int t = 0; t < tiers; ++t) {
    const float h = 12.0f + 30.0f * u(rng);
    appendBox(m, {0.0f, y + 0.5f * h, 0.0f}, {0.5f * w, 0.5f * h, 0.5f * d});
    // Janelas: caixinhas rasas saindo das 4 fachadas, a cada 3,5 m de altura e ~3 m de largura.
    const int floors = int(h / 3.5f), colsX = std::max(1, int(w / 3.0f)), colsZ = std::max(1, int(d / 3.0f));
    for (int f = 0; f < floors; ++f) {
      const float wy = y + 1.8f + 3.5f * float(f);
      for (int i = 0; i < colsX; ++i) {
        const float x = -0.5f * w + (float(i) + 0.5f) * w / float(colsX);
        appendBox(m, {x, wy, 0.5f * d + 0.05f}, {0.6f, 0.8f, 0.05f});
        appendBox(m, {x, wy, -0.5f * d - 0.05f}, {0.6f, 0.8f, 0.05f});
      }
      for (int i = 0; i < colsZ; ++i) {
        const float z = -0.5f * d + (float(i) + 0.5f) * d / float(colsZ);
        appendBox(m, {0.5f * w + 0.05f, wy, z}, {0.05f, 0.8f, 0.6f});
        appendBox(m, {-0.5f * w - 0.05f, wy, z}, {0.05f, 0.8f, 0.6f});
      }
    }
    y += h;
    w *= 0.65f + 0.25f * u(rng);  // recuo
    d *= 0.65f + 0.25f * u(rng);
  }
  appendBox(m, {0.0f, y + 2.0f, 0.0f}, {1.5f, 2.0f, 1.5f});  // caixa d'água
  return m;
}

// Streaming do mundo: 40 x 40 quarteirões (2,4 km) com uma torre ÚNICA por quarteirão (malha própria, milhares de
// triângulos); só as células perto da câmera ficam na GPU. --move anda pela avenida e mostra o carrega/descarrega.
bool buildStreamTest(Scene& s, Camera& camera, const DemoOptions& opt) {
  const int n = int(std::max(4u, std::min(opt.cityBlocks, 64u)));
  const float pitch = 60.0f, half = 0.5f * float(n) * pitch;
  uint32_t cube = s.addMesh(makeCube());
  Material ground;
  ground.baseColor = glm::vec4(0.07f, 0.07f, 0.075f, 1.0f);
  ground.roughness = 0.85f;
  s.addObject(cube, trs({0.0f, -0.5f, 0.0f}, {half + 50.0f, 0.5f, half + 50.0f}), s.addMaterial(ground));
  uint32_t facade[4];
  const glm::vec3 colors[4] = {{0.55f, 0.52f, 0.48f}, {0.4f, 0.45f, 0.5f}, {0.6f, 0.55f, 0.45f}, {0.35f, 0.33f, 0.32f}};
  for (int i = 0; i < 4; ++i) {
    Material m;
    m.baseColor = glm::vec4(colors[i], 1.0f);
    m.roughness = 0.7f;
    facade[i] = s.addMaterial(m);
  }
  for (int bz = 0; bz < n; ++bz)
    for (int bx = 0; bx < n; ++bx) {
      const glm::vec3 c(-half + pitch * (float(bx) + 0.5f), 0.0f, -half + pitch * (float(bz) + 0.5f));
      const uint32_t mesh = s.addMesh(makeTower(uint32_t(bz * n + bx + 1)));
      const uint32_t cell = s.addStreamCell(c + glm::vec3(0.0f, 40.0f, 0.0f), 45.0f);
      s.setMeshCell(mesh, cell);
      s.addObject(mesh, glm::translate(glm::mat4(1.0f), c), facade[(bx * 7 + bz * 3) % 4]);
    }
  s.streamLoadDistance = 300.0f;
  s.streamUnloadDistance = 380.0f;

  s.sunDirection = sunFrom(opt.night ? -10.0f : 30.0f, 140.0f);
  s.sunIlluminance = opt.night ? 0.0f : 100000.0f;
  s.cloudCoverage = 0.3f;
  s.fogDensity = 0.0006f;
  camera.position = glm::vec3(-half + 30.0f, 2.0f, 0.0f);  // na avenida do meio, olhando ao longo dela
  camera.lookAt(camera.position + glm::vec3(1.0f, 0.05f, 0.0f));
  return true;
}

// Altura das montanhas do mundo aberto (m): cadeias de montanhas por fbm com cristas (ridged) e vales largos.
float worldHeight(float x, float z) {
  const float base = fbm(x * 0.00025f + 11.0f, z * 0.00025f + 3.0f, 5);
  const float ridge = 1.0f - std::abs(fbm(x * 0.0009f + 5.0f, z * 0.0009f + 9.0f, 5) * 2.0f - 1.0f);
  const float mountains = std::pow(glm::smoothstep(0.35f, 0.8f, base), 1.5f) * (ridge * ridge) * 900.0f;
  const float hills = (fbm(x * 0.004f, z * 0.004f, 4) - 0.5f) * 40.0f;
  return mountains + hills;
}

// Grade de terreno em [x0, x0+size] x [z0, z0+size] com `res` células, altura worldHeight − drop.
MeshData makeWorldTile(float x0, float z0, float size, uint32_t res, float drop) {
  MeshData m;
  const float step = size / float(res), e = step;
  for (uint32_t j = 0; j <= res; ++j)
    for (uint32_t i = 0; i <= res; ++i) {
      const float x = x0 + float(i) * step, z = z0 + float(j) * step;
      const glm::vec3 n = glm::normalize(glm::vec3(worldHeight(x - e, z) - worldHeight(x + e, z), 2.0f * e,
                                                   worldHeight(x, z - e) - worldHeight(x, z + e)));
      m.vertices.push_back({{x, worldHeight(x, z) - drop, z}, n, {x, z}, {1.0f, 0.0f, 0.0f, 1.0f}});
    }
  for (uint32_t j = 0; j < res; ++j)
    for (uint32_t i = 0; i < res; ++i) {
      const uint32_t a = j * (res + 1) + i, b = a + res + 1;
      m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  return m;
}

// Mundo aberto: 8 x 8 km de montanhas. Perto da câmera, blocos de 256 m com 4 m de resolução carregados pelo
// streaming (até ~1,2 km); ao longe, uma malha grossa do terreno todo (62 m), sempre na memória e um pouco abaixo
// (os blocos detalhados cobrem ela onde existem). Nuvens, névoa e atmosfera completam a distância.
bool buildWorldTest(Scene& s, Camera& camera, const DemoOptions& opt) {
  const float world = 8192.0f, half = 0.5f * world, tile = 256.0f;
  Material ground;
  ground.flags = kMaterialTerrain;
  addTerrainLayers(s);
  const uint32_t mat = s.addMaterial(ground);
  s.addObject(s.addMesh(makeWorldTile(-half, -half, world, 132, 2.0f)), glm::mat4(1.0f), mat);
  const int tiles = int(world / tile);
  for (int tz = 0; tz < tiles; ++tz)
    for (int tx = 0; tx < tiles; ++tx) {
      const float x0 = -half + float(tx) * tile, z0 = -half + float(tz) * tile;
      const uint32_t mesh = s.addMesh(makeWorldTile(x0, z0, tile, 64, 0.0f));
      const glm::vec3 c(x0 + 0.5f * tile, worldHeight(x0 + 0.5f * tile, z0 + 0.5f * tile), z0 + 0.5f * tile);
      s.setMeshCell(mesh, s.addStreamCell(c, tile * 0.75f));
      s.addObject(mesh, glm::mat4(1.0f), mat);
    }
  s.streamLoadDistance = 1000.0f;
  s.streamUnloadDistance = 1200.0f;
  s.streamBudgetMB = 64.0f;
  s.sunDirection = sunFrom(opt.night ? -10.0f : 18.0f, 200.0f);
  s.sunIlluminance = opt.night ? 0.0f : 120000.0f;
  s.moonIlluminance = opt.night && opt.moon ? 0.25f : 0.0f;
  s.cloudCoverage = 0.45f;
  s.fogDensity = 0.00015f;
  s.fogFalloff = 0.004f;
  const float cx = -600.0f, cz = 900.0f;
  camera.position = glm::vec3(cx, worldHeight(cx, cz) + 60.0f, cz);
  camera.lookAt(camera.position + glm::vec3(1.0f, -0.05f, -0.6f));
  return true;
}

// Tecido com colisão: uma toalha de 1,6 m cai sobre uma bola e se acomoda no chão.
bool buildClothTest(Scene& s, Camera& camera) {
  uint32_t sphere = s.addMesh(makeSphere(64, 32)), cube = s.addMesh(makeCube());
  auto mat = [&](glm::vec3 albedo, float rough, uint32_t flags = 0) {
    Material m;
    m.baseColor = glm::vec4(albedo, 1.0f);
    m.roughness = rough;
    m.flags = flags;
    return s.addMaterial(m);
  };
  s.addObject(cube, trs({0.0f, -0.05f, 0.0f}, {3.0f, 0.05f, 3.0f}), mat(glm::vec3(0.4f), 0.8f));
  s.addObject(sphere, trs({0.0f, 0.5f, 0.0f}, glm::vec3(0.5f)), mat({0.1f, 0.3f, 0.7f}, 0.3f));
  MeshData sheet = makeClothSheet(32, 32, 0.05f);  // 1.024 pontos: o limite de um pano (shaders/cloth.comp)
  const float top = 1.3f;
  sheet.clothColliders[0] = glm::vec4(0.0f, 0.5f - top, 0.0f, 0.5f);          // a bola (espaço do pano)
  sheet.clothColliders[1] = glm::vec4(0.0f, -1000.0f - top, 0.0f, 1000.0f);  // o chão, como uma esfera enorme
  s.addObject(s.addMesh(std::move(sheet)), glm::translate(glm::mat4(1.0f), {0.0f, top, 0.0f}),
              mat({0.75f, 0.1f, 0.12f}, 0.85f, kMaterialDoubleSided));
  s.sunDirection = sunFrom(40.0f, 130.0f);
  s.sunIlluminance = 100000.0f;
  s.fogDensity = 0.0f;
  camera.position = glm::vec3(1.8f, 1.4f, 2.4f);
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
  if (opt.name == "river") return buildRiver(scene, camera, opt);
  if (opt.name == "scan" || opt.name == "scan-backlit") return buildScanHead(scene, camera, opt, opt.name == "scan-backlit");
  if (opt.name == "paint") return buildPaintTest(scene, camera);
  if (opt.name == "skin") return buildSkinTest(scene, camera, false);
  if (opt.name == "skin-backlit") return buildSkinTest(scene, camera, true);
  if (opt.name == "vfx") return buildVfx(scene, camera, opt);
  if (opt.name == "crowd") return buildCrowd(scene, camera, opt);
  if (opt.name == "hair") return buildHairTest(scene, camera);
  if (opt.name == "stream") return buildStreamTest(scene, camera, opt);
  if (opt.name == "world") return buildWorldTest(scene, camera, opt);
  if (opt.name == "cloth") return buildClothTest(scene, camera);
  return buildSponza(scene, camera, opt);
}

}  // namespace scene
