#pragma once
// Pênalti da Treta no Legacy Engine: o estádio (cena) e o jogo de pênaltis que roda em cima dele.
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "scene/camera.h"
#include "scene/demo_scene.h"
#include "scene/scene.h"

namespace game {

// Medidas oficiais (m). O gol fica em z = 0, a marca do pênalti em z = 11 e a câmera do batedor atrás dela (+z).
constexpr float kGoalHalfWidth = 3.66f, kGoalHeight = 2.44f, kNetDepth = 1.95f, kSpotZ = 11.0f, kBallRadius = 0.11f;

// Um jogador (personagem com esqueleto): o modelo e os ossos que o jogo mira a cada quadro.
struct Player {
  int model = -1;
  uint32_t bodyMaterial = 0, jointMaterial = 0;
  int hips = -1, spine = -1, head = -1;
  int arm[2] = {-1, -1}, foreArm[2] = {-1, -1}, hand[2] = {-1, -1};      // 0 = direita, 1 = esquerda
  int upLeg[2] = {-1, -1}, leg[2] = {-1, -1}, foot[2] = {-1, -1}, toe[2] = {-1, -1};
  int idle = -1, run = -1, agree = -1;
  // Pose pedida pelo jogo (direções no espaço do mundo; comprimento 0 = deixa o clipe). Aplicada no poseHook.
  glm::vec3 armDir[2]{}, foreArmDir[2]{}, upLegDir[2]{}, legDir[2]{}, footDir[2]{};
  glm::vec3 spineDir{0.0f};
};

// Tudo o que o jogo mexe depois de montar a cena.
struct World {
  Player kicker, keeper;
  uint32_t ball = 0;           // objeto
  uint32_t netMesh = 0;        // malha de tecido do fundo da rede (colisor da bola)
  glm::mat4 netTransform{1.0f};
  uint32_t ledMaterial = 0;    // placas de LED (texto trocado pelo jogo)
  int32_t ledTexture = -1;
  std::vector<uint32_t> crowdClips;
  float crowdCheer = 0.0f;     // 0..1: torcida pulando
};

World& world();  // a cena "penalti" preenche; o jogo lê

// Monta o estádio à noite (refletores, torcida, placas de LED), o gol com rede de tecido, a bola e os dois jogadores.
bool buildPenaltiScene(scene::Scene& s, scene::Camera& camera, const scene::DemoOptions& opt);

void setKit(scene::Scene& s, const Player& p, glm::vec3 shirt, glm::vec3 shorts);

// Texto em pixels (fonte do stb_easy_font) numa imagem RGBA: usado nas placas de LED e no HUD.
void drawText(std::vector<uint8_t>& rgba, int width, int height, const std::string& text, int x, int y, int scale,
              glm::u8vec4 color);

}  // namespace game
