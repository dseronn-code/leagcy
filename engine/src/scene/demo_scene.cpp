#include "scene/demo_scene.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace scene {

float demoTerrainHeight(float x, float z) {
  float dist = std::sqrt(x * x + z * z);
  float hills = (fbm(x * 0.006f + 3.1f, z * 0.006f + 7.7f, 6) - 0.45f) * 90.0f;
  float mask = glm::smoothstep(35.0f, 160.0f, dist);  // área central plana para a vitrine
  float bumps = (fbm(x * 0.12f, z * 0.12f, 3) - 0.5f) * 0.35f;
  return hills * mask + bumps;
}

namespace {
glm::mat4 trs(glm::vec3 t, glm::vec3 s, float yawDeg = 0.0f) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), t);
  m = glm::rotate(m, glm::radians(yawDeg), glm::vec3(0, 1, 0));
  return glm::scale(m, s);
}
}  // namespace

void buildDemoScene(Scene& s, Camera& camera) {
  uint32_t terrain = s.addMesh(makeTerrain(1200.0f, 640, demoTerrainHeight));
  uint32_t sphere = s.addMesh(makeSphere());
  uint32_t cube = s.addMesh(makeCube());

  Material ground;
  ground.type = 1;
  s.addObject({terrain, glm::mat4(1.0f), ground});

  auto groundY = [](float x, float z) { return demoTerrainHeight(x, z); };

  // Galeria de materiais: rugosidade cresce da esquerda para a direita.
  for (int i = 0; i < 7; ++i) {
    float x = -9.0f + i * 3.0f, rough = i / 6.0f;
    Material plastic{{0.70f, 0.08f, 0.06f}, glm::mix(0.05f, 1.0f, rough), 0.0f};
    s.addObject({sphere, trs({x, groundY(x, 0) + 1.0f, 0.0f}, glm::vec3(1.0f)), plastic});
    Material gold{{1.0f, 0.78f, 0.34f}, glm::mix(0.05f, 1.0f, rough), 1.0f};
    s.addObject({sphere, trs({x, groundY(x, -3.5f) + 1.0f, -3.5f}, glm::vec3(1.0f)), gold});
  }

  // Esfera espelhada gigante.
  Material chrome{{0.95f, 0.95f, 0.97f}, 0.02f, 1.0f};
  s.addObject({sphere, trs({-15.0f, groundY(-15, -12) + 4.0f, -12.0f}, glm::vec3(4.0f)), chrome});

  // "Cornell corner": paredes coloridas para mostrar luz rebatida.
  Material white{{0.80f, 0.78f, 0.74f}, 0.6f, 0.0f};
  Material red{{0.75f, 0.06f, 0.05f}, 0.7f, 0.0f};
  Material green{{0.08f, 0.55f, 0.10f}, 0.7f, 0.0f};
  float cx = 14.0f, cz = -10.0f, cy = groundY(cx, cz);
  s.addObject({cube, trs({cx, cy + 3.0f, cz - 4.0f}, {4.0f, 3.0f, 0.15f}), white});
  s.addObject({cube, trs({cx - 4.0f, cy + 3.0f, cz}, {0.15f, 3.0f, 4.0f}), red});
  s.addObject({cube, trs({cx + 4.0f, cy + 3.0f, cz}, {0.15f, 3.0f, 4.0f}), green});
  s.addObject({cube, trs({cx - 1.2f, cy + 1.2f, cz - 1.0f}, {1.0f, 1.2f, 1.0f}, 20.0f), white});
  Material copper{{0.95f, 0.64f, 0.54f}, 0.25f, 1.0f};
  s.addObject({sphere, trs({cx + 1.6f, cy + 1.1f, cz + 0.5f}, glm::vec3(1.1f)), copper});

  // Monólitos para sombras longas.
  Material stone{{0.35f, 0.33f, 0.30f}, 0.85f, 0.0f};
  const glm::vec3 monoliths[] = {{6, 0, -22}, {-4, 0, -30}, {22, 0, -28}, {-26, 0, -4}};
  for (int i = 0; i < 4; ++i) {
    glm::vec3 p = monoliths[i];
    p.y = groundY(p.x, p.z) + 5.0f;
    s.addObject({cube, trs(p, {1.2f, 6.0f, 0.8f}, 25.0f * i), stone});
  }

  // Sol de fim de tarde (golden hour), em contraluz pela direita.
  float elev = glm::radians(16.0f), azim = glm::radians(55.0f);
  s.sunDirection = glm::normalize(glm::vec3(std::cos(elev) * std::sin(azim), std::sin(elev), -std::cos(elev) * std::cos(azim)));
  s.sunIntensity = 20.0f;

  camera.position = {2.0f, groundY(2, 16) + 3.2f, 16.0f};
  camera.lookAt({0.0f, 1.0f, -6.0f});
}

}  // namespace scene
