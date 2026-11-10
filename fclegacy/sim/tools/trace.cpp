// Grava a partida (computador x computador) em CSV: um quadro a cada N passos, com os 22 jogadores e a bola.
// Uso: fcsim_trace semente passos_por_quadro > partida.csv
#include <cstdio>
#include <cstdlib>
#include "fcsim/match.h"
using namespace fcsim;
int main(int argc, char** argv) {
  MatchConfig cfg; cfg.seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1;
  const int every = argc > 2 ? std::atoi(argv[2]) : 3;
  static Match m; initMatch(m, cfg);
  std::printf("tick,phase,owner,score0,score1,bx,by,bz");
  for (int i = 0; i < 22; ++i) std::printf(",p%dx,p%dz", i, i);
  std::printf("\n");
  while (!finished(m)) {
    stepMatch(m, nullptr);
    if (m.tick % every) continue;
    std::printf("%d,%d,%d,%d,%d,%.2f,%.2f,%.2f", m.tick, int(m.phase), m.owner, m.score[0], m.score[1], m.ball.pos.x, m.ball.pos.y, m.ball.pos.z);
    for (const Player& p : m.players) std::printf(",%.2f,%.2f", p.pos.x, p.pos.z);
    std::printf("\n");
  }
}
