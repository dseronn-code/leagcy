#pragma once
// Jogo de pênaltis sobre o estádio da cena "penalti": você contra a IA, 5 cobranças e alternadas, 7 s por escolha.
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "game/penalti.h"
#include "game/ui.h"
#include "renderer/renderer.h"

namespace game {

// Toque/clique em pixels da tela. pressed/released valem só no quadro em que aconteceram.
struct Pointer {
  glm::vec2 pos{0.0f};
  bool down = false, pressed = false, released = false;
};

struct Kicker {
  std::string name;
  int over = 75;
};
struct Team {
  std::string name, abbr;
  glm::vec3 shirt, shorts, keeperShirt;
  std::vector<Kicker> kickers;
};

class PenaltiGame {
 public:
  // autoplay: as escolhas do jogador são feitas sozinhas (vídeos e testes sem tela).
  // rayTracing: o dispositivo tem sombras das luzes; sem ele, sombra de um refletor pelos shadow maps do motor.
  void init(scene::Scene& s, renderer::Renderer& r, bool autoplay, bool rayTracing = true);
  // Avança o jogo `realDt` segundos; atualiza câmera, jogadores, bola e o HUD (r.ui). Devolve o dt da simulação da
  // cena (câmera lenta no lance).
  float update(float realDt, const Pointer& p, scene::Scene& s, scene::Camera& cam, renderer::Renderer& r, glm::vec2 screen);

 private:
  enum class State { Menu, Pass, ChooseKick, ChooseSave, Shot, Result, End };
  struct Choice { int col = 1, row = 1; int grade = 0; };  // grade: índice da faixa do medidor
  struct Outcome { int type = 0; bool goal = false; bool close = false; };

  // regras
  Outcome resolve(const Choice& kick, const std::string& power, const Choice& save, const std::string& reflex) const;
  int decided() const;
  int pressure(int kickTeam) const;
  // medidor
  struct Seg { const char* name; float end; glm::vec4 color; };
  std::vector<Seg> segments(bool kick, int over) const;
  // lance
  void startShot(scene::Scene& s);
  void updateShot(float dt, scene::Scene& s);
  void outcomeFx();
  void resetPoses(scene::Scene& s);
  void poseKeeperReady(float t);
  // câmera
  void cameraAim(scene::Camera& cam, glm::vec2 screen);
  void cameraUpdate(float dt, scene::Camera& cam, glm::vec2 screen);
  glm::vec2 project(const scene::Camera& cam, glm::vec3 p, glm::vec2 screen) const;
  // HUD
  void drawHud(scene::Camera& cam, glm::vec2 screen);
  void drawScoreboard(glm::vec2 screen);
  void drawChoose(scene::Camera& cam, glm::vec2 screen);
  void drawMenu(glm::vec2 screen);
  void drawResult(glm::vec2 screen);
  void drawEnd(glm::vec2 screen);
  void handleInput(const Pointer& p, scene::Camera& cam, glm::vec2 screen);
  void finishChoice(bool timeout);
  void nextTurn(scene::Scene& s);

  Ui ui_;
  bool autoplay_ = false, rayTracing_ = true;
  bool startRequested_ = false, shotPending_ = false;  // pedidos do toque, atendidos no update (que tem a cena)
  State state_ = State::Menu;
  float stateTime_ = 0.0f, simTime_ = 0.0f;
  Team team_[2];  // 0 = você, 1 = IA
  std::vector<int> kicks_[2];
  int first_ = 0, n_ = 5;
  // turno
  int kickTeam_ = 0;
  Kicker kicker_;
  bool zoneChosen_ = false, meterRunning_ = false;
  Choice choice_;
  float meterStart_ = 0.0f, meterSpeed_ = 1.0f;
  Choice kick_, save_;
  std::string power_, reflex_;
  std::vector<int> aiHistory_;
  Outcome outcome_;
  std::string resultWord_, resultLine_;
  glm::vec4 resultColor_{1.0f};
  float autoplayDelay_ = 0.0f;
  // lance em andamento
  struct Shot {
    float t = 0.0f, F = 0.6f, contactS = -1.0f, diveStart = 1.0f, D = 0.42f, outcomeAt = -1.0f;
    glm::vec3 target{0.0f}, ctrl{0.0f}, diveT{0.0f}, vel{0.0f}, ballPos{0.0f};
    glm::quat ballRot{1, 0, 0, 0};
    bool kicked = false, free = false, held = false, hitNet = false, done = false;
  } shot_;
  float timeScale_ = 1.0f, tsTarget_ = 1.0f;
  // câmera
  glm::vec3 camPos_{0.0f, 1.9f, 17.6f}, camLook_{0.0f, 1.0f, 0.0f}, camPosT_{0.0f}, camLookT_{0.0f};
  float camK_ = 3.0f, camFov_ = 50.0f, camFovT_ = 50.0f, shake_ = 0.0f;
  bool camAim_ = true;
};

}  // namespace game
