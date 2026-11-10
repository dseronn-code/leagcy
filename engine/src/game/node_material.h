#pragma once
// Material por nós (no estilo do editor de materiais do Unreal / Substance): um grafo em JSON de geradores (ruído,
// Voronoi, tijolos, listras, gradiente), operações (misturar, multiplicar, somar, contraste, inverter) e rampas de cor,
// avaliado por texel e assado em texturas que repetem sem emenda: cor (sRGB), rugosidade + metal e normal (do relevo).
// Formato em docs/motor_completo.md.
#include <string>

#include <nlohmann/json.hpp>

#include "scene/scene.h"

namespace game {

struct NodeMaterialBake {
  scene::TextureData color, metalRough, normal;  // normal vazio se o grafo não tem "altura"
  bool hasNormal = false;
};

// Assa o grafo `graph` ("nos" + "saida") em `size` x `size`. Falso com `error` se o grafo é inválido.
bool bakeNodeMaterial(const nlohmann::json& graph, uint32_t size, NodeMaterialBake& out, std::string& error);

}  // namespace game
