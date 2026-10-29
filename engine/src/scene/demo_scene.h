#pragma once
#include <string>

#include "scene/camera.h"
#include "scene/scene.h"

namespace scene {

struct DemoOptions {
  std::string name = "sponza";  // sponza, outdoor, furnace, shadows, shadows-far ou arquivo .gltf/.glb
  bool night = false;           // noite: sem sol; luzes locais (vitrine do ReSTIR), lua e estrelas
  bool moon = true;             // à noite, lua cheia (--no-moon: noite sem lua)
  float clouds = -1.0f;         // cobertura de nuvens 0..1 (--clouds); < 0 = padrão da cena
  float fog = -1.0f;            // densidade da névoa no chão (1/m, --fog); < 0 = padrão da cena
  float rain = 0.0f;            // chuva 0..1 (--rain): molhado, poças e respingos
  uint32_t lightCount = 0;      // 0 = padrão da cena
  uint32_t cityBlocks = 16;     // cena "city": quarteirões por lado (16 = ~5 mil objetos; 64 = ~90 mil)
};

// Monta a cena de demonstração escolhida. Retorna false se os assets não existirem.
bool buildDemoScene(Scene& scene, Camera& camera, const DemoOptions& opt);

}  // namespace scene
