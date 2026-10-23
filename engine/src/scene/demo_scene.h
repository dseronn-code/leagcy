#pragma once
#include <string>

#include "scene/camera.h"
#include "scene/scene.h"

namespace scene {

struct DemoOptions {
  std::string name = "sponza";  // sponza, outdoor, furnace, shadows, shadows-far ou arquivo .gltf/.glb
  bool night = false;           // noite: sem sol, só luzes locais (vitrine do ReSTIR)
  uint32_t lightCount = 0;      // 0 = padrão da cena
};

// Monta a cena de demonstração escolhida. Retorna false se os assets não existirem.
bool buildDemoScene(Scene& scene, Camera& camera, const DemoOptions& opt);

}  // namespace scene
