#pragma once
// FCLegacy: o estádio da partida 11 x 11 (campo inteiro, dois gols com rede, arquibancadas, torcida, refletores) e o
// que o jogo mexe depois de montar a cena. A partida em si (física, regras, IA) é o núcleo fcsim (fclegacy/sim).
#include <array>

#include "game/penalti.h"

namespace game {

struct PartidaWorld {
  std::array<Player, 22> players;   // 0-10 time da casa, 11-21 visitante (o 0 e o 11 são os goleiros)
  std::array<Player, 3> officials;  // juiz e os dois bandeirinhas
  // Torcida em recortes: material de cada pessoa do atlas (8) em cada versão de camisa (casa, fora, neutra). O jogo
  // troca o quadro (parado/comemorando) mexendo no deslocamento de UV desses materiais.
  std::vector<uint32_t> crowdMaterials;
  bool day = true;
  uint32_t ball = 0;
  uint32_t netMesh[2] = {0, 0};     // fundo da rede do gol em x = -52,5 e em x = +52,5
  glm::mat4 netTransform[2]{glm::mat4(1.0f), glm::mat4(1.0f)};
  std::map<std::string, ClipMeta> clips;
  float clipFps = 30.0f;
  uint32_t ledMaterial = 0;
  int32_t ledTexture = -1;
};

PartidaWorld& partidaWorld();
bool buildPartidaScene(scene::Scene& s, scene::Camera& camera, const scene::DemoOptions& opt);

}  // namespace game
