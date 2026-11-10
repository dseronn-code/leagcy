#pragma once
// Pênalti da Treta no Legacy Engine: o estádio (cena) e o jogo de pênaltis que roda em cima dele.
#include <map>
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

  // Jogador realista (game/jogador.glb): clipes de captura de movimento tocados por camadas, com transição.
  bool real = false;
  glm::mat4 fix{1.0f};          // escala e altura do modelo (1,82 m, pés em y = 0)
  std::vector<uint32_t> shirtMats, shortsMats;
  struct Anim {
    std::string cur, prev;
    float tCur = 0.0f, tPrev = 0.0f, fade = 1.0f, fadeDur = 0.25f;
    bool once = false;
    glm::vec3 base{0.0f};       // contêiner: o clipe soma o próprio deslocamento (meta.root)
    float yaw = 0.0f, off = 0.0f, offT = 0.0f, lift = 0.0f, liftT = 0.0f;
    glm::vec3 reach{0.0f};      // mira dos braços (defesa)
    float reachW = 0.0f;
  } anim;
};

// Dados dos clipes do jogador realista (game/jogador_anim.json): deslocamento no campo por quadro e eventos.
struct ClipMeta {
  float dur = 0.0f;
  std::vector<glm::vec2> root;  // x, z por quadro (fps)
  float contact = -1.0f;        // k_kick: pé na bola
  glm::vec3 foot{0.0f};         // k_kick: bico do pé no contato (espaço do jogador)
  float speed = 0.0f;           // locomoção: velocidade (m/s) em que o clipe foi gravado
  float reachT = -1.0f;         // goleiro: instante e ponto das mãos
  glm::vec2 reach{0.0f};
};

// Tudo o que o jogo mexe depois de montar a cena.
struct PenaltiWorld {
  Player kicker, keeper;
  uint32_t ball = 0;           // objeto
  uint32_t netMesh = 0;        // malha de tecido do fundo da rede (colisor da bola)
  glm::mat4 netTransform{1.0f};
  uint32_t ledMaterial = 0;    // placas de LED (texto trocado pelo jogo)
  int32_t ledTexture = -1;
  std::vector<uint32_t> crowdClips;
  float crowdCheer = 0.0f;     // 0..1: torcida pulando
  std::map<std::string, ClipMeta> clips;
  float clipFps = 30.0f;
};

PenaltiWorld& world();  // a cena "penalti" preenche; o jogo lê

// Monta o estádio à noite (refletores, torcida, placas de LED), o gol com rede de tecido, a bola e os dois jogadores.
bool buildPenaltiScene(scene::Scene& s, scene::Camera& camera, const scene::DemoOptions& opt);

void setKit(scene::Scene& s, const Player& p, glm::vec3 shirt, glm::vec3 shorts);
// Lê game/jogador_anim.json (eventos e deslocamento dos clipes do jogador realista).
bool loadClipMetaInto(std::map<std::string, ClipMeta>& clips, float& fps);

// Texto em pixels (fonte do stb_easy_font) numa imagem RGBA: usado nas placas de LED e no HUD.
void drawText(std::vector<uint8_t>& rgba, int width, int height, const std::string& text, int x, int y, int scale,
              glm::u8vec4 color);

}  // namespace game
