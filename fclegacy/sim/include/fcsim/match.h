#pragma once
// Partida do FCLegacy: 22 jogadores, bola, regras e IA, num passo fixo de 30 Hz. Determinística: o estado depende só
// da configuração (semente) e das entradas de cada passo. Sem trigonometria da libm (só soma, multiplicação e raiz,
// que são exatas no IEEE): o mesmo resultado em qualquer aparelho com o mesmo binário.
//
// Campo: x no comprimento (gols em x = ±52,5), z na largura (laterais em z = ±34), y para cima.
// O time 0 ataca +x no 1º tempo; os lados trocam no intervalo.
#include <array>
#include <cstdint>

#include "fcsim/ball.h"

namespace fcsim {

constexpr int kTicksPerSecond = 30;
constexpr float kTick = 1.0f / float(kTicksPerSecond);
constexpr float kHalfLength = 52.5f, kHalfWidth = 34.0f;
constexpr float kGoalHalfWidth = 3.66f, kGoalHeight = 2.44f;
constexpr int kPlayersPerTeam = 11;

struct Rng {
  uint64_t s = 0x9E3779B97F4A7C15ull;
  uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 2685821657736338717ull; }
  float uniform() { return float(next() >> 40) / float(1ull << 24); }       // [0, 1)
  float signedUnit() { return uniform() * 2.0f - 1.0f; }                     // [-1, 1)
  float gauss() { return (uniform() + uniform() + uniform() + uniform() - 2.0f) * 0.866f; }  // ~N(0,1) aproximada
};

// Atributos de 0 a 1 (0,5 = profissional médio; 0,9 = craque). Velocidades em m/s.
struct Attributes {
  float topSpeed = 7.8f, accel = 5.5f;
  float passing = 0.6f, shooting = 0.6f, dribbling = 0.6f, defending = 0.6f, keeping = 0.3f, stamina = 0.7f;
};

enum class Role : uint8_t { Goalkeeper, Defender, Midfielder, Forward };

struct Player {
  int team = 0, index = 0;
  Role role = Role::Midfielder;
  Vec3 home{};        // posição na formação (time atacando +x, metade própria em x < 0)
  Vec3 pos{}, vel{};
  Vec3 facing{1.0f, 0.0f, 0.0f};  // direção do corpo (unitário, plano xz)
  Attributes attr;
  float energy = 1.0f;            // 1 = descansado
  int cooldown = 0;               // passos até poder tocar na bola de novo
  int touchTimer = 0;             // condução: passos até o próximo toque
};

// Entrada de um time num passo (humano ou roteiro). Direção quantizada em 8 bits por eixo (pronta para a rede).
struct Input {
  int8_t dirX = 0, dirZ = 0;  // stick (x = para o gol adversário do ponto de vista da câmera é tratado no jogo)
  uint8_t buttons = 0;        // bits abaixo (estado "segurando")
  // Lob: gesto para cima/baixo no botão (passe alto, cruzamento, enfiada por cima). Special: deslizar para o lado
  // (chute colocado no ataque; na defesa, chama o 2º homem para pressionar), como os gestos do eFootball.
  enum : uint8_t { Pass = 1, Through = 2, Shoot = 4, Sprint = 8, Lob = 16, Press = 32, Switch = 64, Special = 128 };
};

enum class Phase : uint8_t { Kickoff, Play, ThrowIn, Corner, GoalKick, Goal, HalfTime, FullTime };

struct TeamStats {
  int shots = 0, onTarget = 0, passes = 0, passesCompleted = 0, passesIntercepted = 0, passesOut = 0, tackles = 0, saves = 0, corners = 0;
  int possessionTicks = 0;
  float xg = 0.0f;
};

struct MatchConfig {
  uint64_t seed = 1;
  float halfSeconds = 180.0f;   // 3 min de relógio real por tempo (45 min de jogo)
  int humanTeam = -1;           // -1 = computador x computador
  Attributes teamAttr[2];       // atributo base de cada time (os jogadores variam em torno dele)
};

struct Match {
  MatchConfig cfg;
  BallState ball;
  BallParams ballParams;
  std::array<Player, 2 * kPlayersPerTeam> players;
  int score[2] = {0, 0};
  TeamStats stats[2];
  Phase phase = Phase::Kickoff;
  int half = 1;
  int tick = 0, halfTick = 0, stoppageTicks = 0, phaseTicks = 0;
  int owner = -1;          // jogador com a bola dominada (-1 = solta)
  int lastTouch = -1;      // último a tocar (regras de saída)
  int restartTeam = 0;     // quem repõe a bola
  int controlled[2] = {-1, -1};  // jogador sob controle humano de cada time
  int aiThinkTimer[2] = {0, 0};
  int pendingPasser = -1;  // passe em andamento (para contar passes completos)
  int passTarget = -1;     // quem deve receber o passe em andamento
  int possessionAge = 0;   // passos desde que o portador dominou a bola
  // chute em andamento: onde a bola deve cruzar a linha do gol (para o goleiro reagir)
  int shotTeam = -1, shotTick = 0;
  Vec3 shotCross{};
  Rng rng;
  // Últimas ações de cada jogador (para a animação): passo em que aconteceu e o tipo.
  enum Action : uint8_t { ActPass, ActShot, ActHeader, ActSave, ActTackle, ActLob };
  std::array<int, 2 * kPlayersPerTeam> actionTick{};
  std::array<uint8_t, 2 * kPlayersPerTeam> actionType{};
  // Barra de força do humano (eFootball): enche em 1 s segurando Passe/Lançamento/Chute; dispara ao soltar ou cheia.
  int charge[2] = {0, 0};
  uint8_t chargeButton[2] = {0, 0};  // qual botão está carregando (0 = nenhum)
  bool chargeFired[2] = {false, false};  // já disparou com a barra cheia: espera soltar
  uint8_t prevButtons[2] = {0, 0};
  // Passe/chute de primeira: comandado antes de a bola chegar, executado no domínio.
  uint8_t queued[2] = {0, 0};
  float queuedPower[2] = {0.0f, 0.0f};
  Vec3 queuedDir[2]{};
  bool queuedLob[2] = {false, false}, queuedSpecial[2] = {false, false};
  // Direção e arrancada do humano neste passo (orientam o primeiro toque no domínio).
  Vec3 humanStick[2]{};
  bool humanDash[2] = {false, false};
  bool callPress[2] = {false, false};  // 2º homem pressionando o portador (gesto na Pressão)
  int switchCooldown[2] = {0, 0};      // troca automática de jogador: espera entre trocas (sem pular)
  Vec3 prevBallPos{};                  // posição da bola no passo anterior (interceptação contínua)
  float chargeLevel(int team) const { return float(charge[team]) / float(kTicksPerSecond); }

  // Sentido de ataque do time neste tempo (+1 ataca +x, -1 ataca -x).
  float attackDir(int team) const { return ((team == 0) == (half == 1)) ? 1.0f : -1.0f; }
  // Minuto de jogo (0-90 + acréscimos) para o placar.
  float gameMinute() const;
};

void initMatch(Match& m, const MatchConfig& cfg);
// Um passo de 1/30 s. `inputs` pode ser nulo (sem humanos).
void stepMatch(Match& m, const Input* inputs);
bool finished(const Match& m);
uint64_t hashMatch(const Match& m);

}  // namespace fcsim
