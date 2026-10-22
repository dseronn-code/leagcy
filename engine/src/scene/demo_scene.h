#pragma once
#include "scene/camera.h"
#include "scene/scene.h"

namespace scene {

// Cena de vitrine: terreno procedural, galeria de materiais PBR, espelho e "Cornell corner"
// para evidenciar sombras suaves, GI com sangramento de cor e reflexões traçadas por raio.
void buildDemoScene(Scene& scene, Camera& camera);
float demoTerrainHeight(float x, float z);

}  // namespace scene
