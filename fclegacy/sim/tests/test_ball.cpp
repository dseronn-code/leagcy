// Testes da física da bola: números de referência (FIFA, papers) e determinismo.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "fcsim/ball.h"

using namespace fcsim;

namespace {
int failures = 0;
void check(bool ok, const char* name, const char* fmt, double v) {
  std::printf("%-34s %s  ", name, ok ? "OK  " : "FALHOU");
  std::printf(fmt, v);
  std::printf("\n");
  if (!ok) ++failures;
}
constexpr float kPi = 3.14159265358979f;

// Chute a partir do chão: velocidade, ângulo vertical (graus) e spin (rad/s).
BallState kick(float speed, float elevDeg, Vec3 spin = {}) {
  BallState b;
  const float e = elevDeg * kPi / 180.0f;
  b.vel = {0.0f, speed * std::sin(e), speed * std::cos(e)};
  b.spin = spin;
  b.onGround = false;
  return b;
}
}  // namespace

int main() {
  const BallParams P;

  // 1) FIFA Quality Programme: solta de 2 m (base da bola) -> quica 0,60-0,85 m.
  {
    BallState b;
    b.pos = {0.0f, 2.0f + P.radius, 0.0f};
    b.onGround = false;
    float peak = 0.0f;
    bool bounced = false;
    for (int i = 0; i < 240 * 4; ++i) {
      const int nb = substepBall(b, P);
      if (nb > 0) { if (bounced) break; bounced = true; peak = 0.0f; }
      if (bounced) peak = std::max(peak, b.pos.y - P.radius);
    }
    check(peak >= 0.60f && peak <= 0.85f, "quique de 2 m (FIFA 0,60-0,85 m)", "%.3f m", peak);
  }

  // 2) Rolagem: bola rolando a 3,4 m/s (base de uma rampa de ~1 m, hipótese) para entre 4 e 8 m (FIFA: 4-8 m).
  {
    BallState b;
    b.vel = {0.0f, 0.0f, 3.43f};
    b.spin = cross(Vec3{0, 1, 0}, b.vel) / P.radius;
    float t = 0.0f;
    while (length(b.vel) > 0.01f && t < 30.0f) { substepBall(b, P); t += kBallSubstep; }
    check(b.pos.z >= 4.0f && b.pos.z <= 8.0f, "rolagem a 3,4 m/s (4-8 m)", "%.2f m", b.pos.z);
  }

  // 3) Passe rasteiro forte de chapa (15 m/s, já rolando): chega a 30 m em 2-3,6 s. Faixa estimada (sem fonte):
  //    calibrar com tempos de passe medidos em partidas.
  {
    BallState b;
    b.vel = {0.0f, 0.0f, 15.0f};
    b.spin = cross(Vec3{0, 1, 0}, b.vel) / P.radius;
    float t = 0.0f;
    while (b.pos.z < 30.0f && t < 10.0f) { substepBall(b, P); t += kBallSubstep; }
    check(t > 2.0f && t < 3.6f, "passe rasteiro 15 m/s: 30 m em", "%.2f s", t);
  }

  // 4) Chute longo: 30 m/s a 20°, sem spin. No vácuo iria a ~59 m; com o ar, menos.
  {
    BallState b = kick(30.0f, 20.0f);
    for (int i = 0; i < 240 * 6 && !b.onGround; ++i) if (substepBall(b, P) > 0) break;
    check(b.pos.z > 38.0f && b.pos.z < 55.0f, "chute 30 m/s a 20°: primeiro quique", "%.1f m", b.pos.z);
  }

  // 5) Falta com efeito: 25 m/s, 12°, spin lateral de 8 voltas/s -> curva de 1 a 4 m em 25 m.
  {
    BallState b = kick(25.0f, 12.0f, {0.0f, 8.0f * 2.0f * kPi, 0.0f});
    while (b.pos.z < 25.0f && !b.onGround) substepBall(b, P);
    check(std::fabs(b.pos.x) > 1.0f && std::fabs(b.pos.x) < 4.0f, "falta com curva: desvio em 25 m", "%.2f m", std::fabs(b.pos.x));
  }

  // 6) Spin no ar cai ~2% em 0,8 s (5,8 -> 5,7 voltas/s).
  {
    BallState b = kick(25.0f, 30.0f, {0.0f, 5.8f * 2.0f * kPi, 0.0f});
    const float w0 = length(b.spin);
    stepBall(b, P, 0.8f);
    const float drop = 1.0f - length(b.spin) / w0;
    check(drop > 0.01f && drop < 0.03f, "queda do spin em 0,8 s (~2%)", "%.4f", drop);
  }

  // 7) Determinismo: mesma simulação duas vezes, e passos agrupados de jeitos diferentes -> mesmos bits.
  {
    auto run = [&](int chunk) {
      BallState b = kick(27.0f, 18.0f, {3.0f, 40.0f, -2.0f});
      for (int i = 0; i < 240 * 8 / chunk; ++i) stepBall(b, P, kBallSubstep * float(chunk));
      return hashBall(b);
    };
    const uint64_t a = run(1), c = run(1), d = run(8);
    check(a == c && a == d, "determinismo (bits iguais)", "%g", double(a == c && a == d));
  }

  std::printf("\n%s (%d falha%s)\n", failures ? "FALHOU" : "TUDO CERTO", failures, failures == 1 ? "" : "s");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
