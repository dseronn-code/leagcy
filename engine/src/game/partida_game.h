#pragma once
// FCLegacy: a partida 11 x 11 jogável no Legacy Engine. A simulação (física da bola, regras, IA) é o núcleo fcsim,
// determinístico a 30 Hz; aqui ficam o desenho (22 jogadores realistas animados por captura de movimento, juiz e
// bandeirinhas, bola, rede, torcida), a câmera de transmissão, os controles por toque (joystick fixo + 4 botões,
// vários dedos ao mesmo tempo, pausa e jogo automático) e o HUD no estilo de transmissão (placar, nomes, minimapa).
#include <array>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "fcsim/match.h"
#include "game/partida.h"
#include "game/ui.h"
#include "renderer/renderer.h"

namespace game {

// Um dedo na tela (pixels). Vários ao mesmo tempo: joystick com um, botões com outro.
struct Touch {
  int id = 0;
  glm::vec2 pos{0.0f};
};

class PartidaGame {
 public:
  // autoplay: computador x computador, começa sozinho (vídeos e testes sem tela).
  void init(scene::Scene& s, renderer::Renderer& r, bool autoplay, bool rayTracing);
  // Avança `realDt` segundos com os dedos que estão na tela agora; desenha o HUD em r.ui. Devolve o dt da cena.
  float update(float realDt, const std::vector<Touch>& touches, scene::Scene& s, scene::Camera& cam, renderer::Renderer& r,
               glm::vec2 screen);

 private:
  enum class Screen { Menu, Playing, End };
  struct Anim {
    float phase = 0.0f;           // fase normalizada da locomoção (0..1)
    int lastAction = -1;          // passo da última ação já tocada
    std::string action;           // clipe de ação em andamento ("" = só locomoção)
    float actionT = 0.0f, actionEnd = 0.0f, actionW = 0.0f;
    float yaw = 0.0f;
    glm::vec3 prevPos{0.0f}, pos{0.0f}, vel{0.0f};
    bool celebrate = false;
  };
  struct Button {
    const char* label;
    const char* defenseLabel;
    uint8_t bit, defenseBit;
    glm::vec2 c;
    float r;
  };
  struct TeamInfo {
    std::string name, abbr;
    glm::vec3 shirt, shorts, keeper;
    std::array<std::string, 11> players;
  };

  void loadTeams();
  void startMatch(scene::Scene& s);
  void readTouches(const std::vector<Touch>& touches, glm::vec2 screen);
  void layoutButtons(glm::vec2 screen);
  void animateBody(scene::Scene& s, Player& P, Anim& a, glm::vec3 pos, glm::vec3 vel, float targetYaw, bool keeper, float dt);
  void animatePlayers(scene::Scene& s, float dt, float alpha);
  void moveOfficials(float dt);
  void drawBall(scene::Scene& s, float dt, float alpha);
  void crowdCheer(scene::Scene& s, bool cheer);
  void cameraUpdate(float dt, scene::Camera& cam, glm::vec2 screen);
  glm::vec2 project(const scene::Camera& cam, glm::vec3 p, glm::vec2 screen) const;
  void drawHud(const scene::Camera& cam, glm::vec2 screen);
  void drawGroundRing(const scene::Camera& cam, glm::vec2 screen, glm::vec3 at, float radius, glm::vec4 color, float thickness);
  void drawMinimap(glm::vec2 screen);

  Ui ui_;
  bool autoplay_ = false;
  Screen screen_ = Screen::Menu;
  float screenTime_ = 0.0f;
  fcsim::Match match_;
  float acc_ = 0.0f;
  std::array<Anim, 22> anim_;
  std::array<Anim, 3> officials_;
  glm::vec3 ballPrev_{0.0f}, ballPos_{0.0f};
  glm::quat ballRot_{1, 0, 0, 0};
  // entrada
  fcsim::Input input_;
  int stickTouch_ = -1;
  glm::vec2 stickBase_{0.0f}, stickVec_{0.0f};
  std::vector<Button> buttons_;
  std::vector<int> prevTouchIds_;
  bool tapped_ = false, paused_ = false, autoTeam_ = false;
  glm::vec2 pauseBtn_{0.0f}, autoBtn_{0.0f};
  float topBtnR_ = 0.0f;
  // câmera
  glm::vec3 camTarget_{0.0f};
  bool camInit_ = false;
  // times
  TeamInfo teams_[2];
  int lastScore_[2] = {0, 0};
  bool cheering_ = false;
  float fps_ = 0.0f;  // média móvel dos quadros por segundo (mostrada no HUD)
};

}  // namespace game
