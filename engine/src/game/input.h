#pragma once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace game {

// Entrada do jogo neste quadro: teclado, mouse e controle (gamepad), como os scripts e o jogador veem.
// Nomes das teclas: "a".."z", "0".."9", "espaco", "esc", "enter", "tab", "shift", "ctrl", "alt", "backspace",
// "cima", "baixo", "esquerda", "direita", "f1".."f12". Botões do controle (layout Xbox): "pad_a", "pad_b", "pad_x",
// "pad_y", "pad_lb", "pad_rb", "pad_back", "pad_start", "pad_l3", "pad_r3", "pad_cima", "pad_baixo",
// "pad_esquerda", "pad_direita". Eixos: esquerdo_x, esquerdo_y, direito_x, direito_y (−1..1, y para baixo),
// lt, rt (0..1).
struct GameInput {
  glm::vec2 mouse{0.5f};  // fração da tela, (0, 0) no canto de cima à esquerda
  bool mouseDown = false, mousePressed = false;
  std::unordered_set<std::string> held, pressed;  // seguradas / apertadas neste quadro
  float axes[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  bool pad = false;  // há controle ligado
  bool down(const std::string& k) const { return held.count(k) > 0; }
  bool hit(const std::string& k) const { return pressed.count(k) > 0; }
  float axis(const std::string& name) const;
  // Estado de entradas simuladas (--simular): teclas seguradas até um instante, eixos fixados.
  std::vector<std::pair<std::string, float>> simHolds;
  std::unordered_map<int, float> simAxes;
  bool simMouse = false;  // houve mouse simulado: a posição do mouse de verdade deixa de valer
};

// Entrada crua do quadro (do GLFW na janela; vazia sem janela).
struct RawInput {
  std::unordered_set<std::string> held;
  glm::vec2 mouse{-1.0f};  // < 0: sem mouse (mantém a última posição)
  bool mouseDown = false;
  bool pad = false;
  float axes[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
};

// Entradas simuladas para testes e demonstrações (--simular "t:evento;..."), em segundos de execução:
//   t:tecla:nome            aperta e solta (um quadro)        t:segurar:nome:duração   segura por `duração` s
//   t:clique:x,y            clique do mouse em (x, y)         t:mouse:x,y              move o mouse
//   t:eixo:nome:valor       fixa um eixo do controle (até outro evento do mesmo eixo)
struct SimEvent {
  float t = 0.0f;
  std::string kind, name;
  glm::vec2 pos{0.0f};
  float value = 0.0f;
};
std::vector<SimEvent> parseSimulated(const std::string& spec);

// Monta a entrada do quadro: o que está seguro agora (cru + simulado), o que foi apertado desde o quadro anterior,
// e os eventos simulados com t em (tPrev, tNow].
void updateInput(GameInput& in, const RawInput& raw, const std::vector<SimEvent>& sim, float tPrev, float tNow);

int axisIndex(const std::string& name);  // -1 se não existe

}  // namespace game
