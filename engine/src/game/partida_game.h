#pragma once
// FCLegacy: a partida 11 x 11 jogável no Legacy Engine. A simulação (física da bola, regras, IA) é o núcleo fcsim,
// determinístico a 30 Hz; aqui ficam o desenho (22 jogadores realistas animados por captura de movimento, bola,
// rede), a câmera de transmissão, os controles por toque (joystick + botões, vários dedos ao mesmo tempo) e o HUD.
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
    glm::vec3 prevPos{0.0f}, pos{0.0f};
    bool celebrate = false;
  };
  struct Button {
    const char* label;
    const char* defenseLabel;
    uint8_t bit, defenseBit;
    glm::vec2 c;
    float r;
  };

  void startMatch(scene::Scene& s);
  void readTouches(const std::vector<Touch>& touches, glm::vec2 screen);
  void layoutButtons(glm::vec2 screen);
  void animatePlayers(scene::Scene& s, float dt, float alpha);
  void drawBall(scene::Scene& s, float dt, float alpha);
  void cameraUpdate(float dt, scene::Camera& cam, glm::vec2 screen);
  glm::vec2 project(const scene::Camera& cam, glm::vec3 p, glm::vec2 screen) const;
  void drawHud(const scene::Camera& cam, glm::vec2 screen);

  Ui ui_;
  bool autoplay_ = false;
  Screen screen_ = Screen::Menu;
  float screenTime_ = 0.0f;
  fcsim::Match match_;
  float acc_ = 0.0f;
  std::array<Anim, 22> anim_;
  glm::vec3 ballPrev_{0.0f}, ballPos_{0.0f};
  glm::quat ballRot_{1, 0, 0, 0};
  // entrada
  fcsim::Input input_;
  int stickTouch_ = -1;
  glm::vec2 stickBase_{0.0f}, stickVec_{0.0f};
  std::vector<Button> buttons_;
  std::vector<int> prevTouchIds_;
  bool tapped_ = false;
  // câmera
  glm::vec3 camTarget_{0.0f};
  bool camInit_ = false;
  // times
  struct TeamInfo { std::string name, abbr; glm::vec3 shirt, shorts, keeper; };
  TeamInfo teams_[2];
  int goalBanner_ = 0;
  float bannerTime_ = 0.0f;
  int lastScore_[2] = {0, 0};
};

}  // namespace game
