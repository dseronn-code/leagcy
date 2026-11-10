// Partida inteira computador x computador: regras, estatísticas plausíveis e determinismo.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "fcsim/match.h"

using namespace fcsim;

namespace {
int failures = 0;
void check(bool ok, const char* name, const char* detail) {
  std::printf("%-40s %s  %s\n", name, ok ? "OK  " : "FALHOU", detail);
  if (!ok) ++failures;
}

struct Summary { int score[2]; TeamStats st[2]; uint64_t hash; int ticks; float maxOut; };

Summary play(uint64_t seed) {
  MatchConfig cfg;
  cfg.seed = seed;
  cfg.teamAttr[0].passing = 0.7f; cfg.teamAttr[0].shooting = 0.65f;  // time um pouco melhor
  static Match m;  // grande: fora da pilha
  initMatch(m, cfg);
  float maxOut = 0.0f;
  while (!finished(m) && m.tick < 30 * 60 * 12) {
    stepMatch(m, nullptr);
    maxOut = std::max(maxOut, std::max(std::fabs(m.ball.pos.x) - kHalfLength, std::fabs(m.ball.pos.z) - kHalfWidth));
  }
  Summary s{};
  s.score[0] = m.score[0]; s.score[1] = m.score[1];
  s.st[0] = m.stats[0]; s.st[1] = m.stats[1];
  s.hash = hashMatch(m); s.ticks = m.tick; s.maxOut = maxOut;
  return s;
}
}  // namespace

int main() {
  char buf[256];
  int goals = 0, shots = 0, passes = 0, completed = 0, matches = 0;
  for (uint64_t seed = 1; seed <= 6; ++seed) {
    const Summary s = play(seed);
    const int poss = s.st[0].possessionTicks + s.st[1].possessionTicks;
    std::printf("semente %llu: %d x %d | chutes %d-%d (no gol %d-%d) xG %.2f-%.2f | passes %d/%d (cortados %d, fora %d) - %d/%d | posse %.0f%% | desarmes %d-%d | defesas %d-%d | escanteios %d-%d | %.1f min\n",
                (unsigned long long)seed, s.score[0], s.score[1], s.st[0].shots, s.st[1].shots, s.st[0].onTarget, s.st[1].onTarget,
                s.st[0].xg, s.st[1].xg, s.st[0].passesCompleted, s.st[0].passes, s.st[0].passesIntercepted, s.st[0].passesOut, s.st[1].passesCompleted, s.st[1].passes,
                poss ? 100.0f * s.st[0].possessionTicks / poss : 0.0f, s.st[0].tackles, s.st[1].tackles, s.st[0].saves, s.st[1].saves,
                s.st[0].corners, s.st[1].corners, s.ticks / 30.0f / 60.0f);
    goals += s.score[0] + s.score[1];
    shots += s.st[0].shots + s.st[1].shots;
    passes += s.st[0].passes + s.st[1].passes;
    completed += s.st[0].passesCompleted + s.st[1].passesCompleted;
    ++matches;
    std::snprintf(buf, sizeof(buf), "%.1f min, bola fora no máximo %.1f m", s.ticks / 1800.0f, s.maxOut);
    if (seed == 1) {
      check(s.ticks >= 6 * 1800 && s.ticks < 8 * 1800, "partida termina em 6-8 min de relógio", buf);
      check(s.maxOut < 3.0f, "bola não foge do campo", buf);
    }
  }
  std::snprintf(buf, sizeof(buf), "%.1f gols/jogo", goals / float(matches));
  check(goals / float(matches) >= 0.8f && goals / float(matches) <= 7.0f, "gols por jogo plausíveis (0,8-7)", buf);
  std::snprintf(buf, sizeof(buf), "%.1f chutes/jogo", shots / float(matches));
  check(shots / float(matches) >= 6.0f && shots / float(matches) <= 45.0f, "chutes por jogo plausíveis (6-45)", buf);
  std::snprintf(buf, sizeof(buf), "%.0f passes/jogo, %.0f%% certos", passes / float(matches), 100.0f * completed / std::max(1, passes));
  check(passes / float(matches) >= 60.0f && completed / float(std::max(1, passes)) >= 0.55f && completed / float(std::max(1, passes)) <= 0.95f,
        "passes: volume e acerto plausíveis", buf);
  const Summary a = play(7), b = play(7);
  check(a.hash == b.hash, "determinismo da partida inteira", a.hash == b.hash ? "mesmos bits" : "diferente");
  std::printf("\n%s (%d falha%s)\n", failures ? "FALHOU" : "TUDO CERTO", failures, failures == 1 ? "" : "s");
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
