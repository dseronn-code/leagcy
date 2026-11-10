#pragma once
// Física da bola do FCLegacy: voo com arrasto (com a "crise de arrasto" das bolas modernas) e efeito Magnus, spin que
// decai devagar, quique na grama com troca entre spin e velocidade, deslizamento e rolagem.
//
// Referências (ver fclegacy/pesquisa/04_tecnologia.md):
// - C_D ~0,45 abaixo e ~0,15-0,2 acima de ~15-17 m/s (Goff & Carré 2009; Hong/Asai, Sci. Rep. 4:5068).
// - Spin cai ~2% em 0,8 s em chute com curva (MDPI Proceedings 49(1):88) -> constante de tempo ~40 s.
// - FIFA Quality Programme (Test Manual II, 2024): bola solta de 2 m quica 0,60-0,85 m na grama (COR ~0,55-0,65).
// Os coeficientes de sustentação (Magnus) e de rolagem são estimativas calibráveis, marcadas abaixo.
//
// Eixos: y para cima, campo no plano xz, chão em y = 0. Unidades SI.
#include <cstdint>

#include "fcsim/vec.h"

namespace fcsim {

struct BallParams {
  float radius = 0.11f;            // m (circunferência 68-70 cm, Lei 2)
  float mass = 0.43f;              // kg (410-450 g)
  float airDensity = 1.2f;         // kg/m³
  float cdLow = 0.45f;             // arrasto abaixo da crise
  float cdHigh = 0.17f;            // arrasto acima da crise (Brazuca ~0,15-0,16; Jabulani ~0,20)
  float crisisFrom = 12.0f;        // m/s: começo da transição
  float crisisTo = 18.0f;          // m/s: fim da transição
  float liftPerSpin = 0.9f;        // C_L = min(liftMax, liftPerSpin * S), S = r|w|/|v|  [ESTIMATIVA — calibrar]
  float liftMax = 0.35f;
  float spinTau = 40.0f;           // s: constante de tempo do spin no ar
  float restitution = 0.62f;       // COR normal na grama seca (FIFA: 0,55-0,65)
  float bounceFriction = 0.45f;    // atrito de Coulomb no quique
  float slideFriction = 0.40f;     // atrito cinético enquanto a bola desliza no gramado
  float rollDecel = 1.0f;          // m/s²: resistência à rolagem na grama [ESTIMATIVA — calibrar pelo teste de rampa]
  float settleSpeed = 0.35f;       // m/s: abaixo disso, o quique para e a bola fica no chão
};

struct BallState {
  Vec3 pos{0.0f, 0.11f, 0.0f};
  Vec3 vel{};
  Vec3 spin{};  // velocidade angular (rad/s), regra da mão direita
  bool onGround = true;
};

// Avança dt segundos (passo interno fixo de 1/240 s; dt deve ser múltiplo dele para o resultado não depender de
// como os passos são agrupados). Devolve quantos quiques houve.
int stepBall(BallState& b, const BallParams& p, float dt);

// Um único passo interno (1/240 s).
constexpr float kBallSubstep = 1.0f / 240.0f;
int substepBall(BallState& b, const BallParams& p);

// Impressão digital do estado (bits exatos), para comparar simulações (rede, replays, testes).
uint64_t hashBall(const BallState& b);

}  // namespace fcsim
