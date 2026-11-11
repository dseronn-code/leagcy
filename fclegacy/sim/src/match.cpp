#include "fcsim/match.h"

#include <algorithm>
#include <cstring>

namespace fcsim {

namespace {

constexpr float kG = 9.81f;
constexpr int kKickCooldown = 9;       // passos sem tocar de novo depois de chutar/passar
constexpr int kRestartPause = 36;      // 1,2 s de bola parada antes de repor

float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
Vec3 flat(Vec3 v) { v.y = 0.0f; return v; }
float dist2(const Vec3& a, const Vec3& b) { const float dx = a.x - b.x, dz = a.z - b.z; return dx * dx + dz * dz; }
float distXZ(const Vec3& a, const Vec3& b) { return std::sqrt(dist2(a, b)); }
int teamOf(int i) { return i / kPlayersPerTeam; }
int other(int t) { return 1 - t; }

// Formação 4-3-3: forma relativa ao centro do bloco (sx: + para o ataque) e largura (sz).
struct Shape { Role role; float sx, sz; };
const Shape kShape[kPlayersPerTeam] = {
    {Role::Goalkeeper, 0.0f, 0.0f},
    {Role::Defender, -14.0f, -21.0f}, {Role::Defender, -16.0f, -7.0f}, {Role::Defender, -16.0f, 7.0f}, {Role::Defender, -14.0f, 21.0f},
    {Role::Midfielder, -3.0f, -14.0f}, {Role::Midfielder, -6.0f, 0.0f}, {Role::Midfielder, -3.0f, 14.0f},
    {Role::Forward, 12.0f, -20.0f}, {Role::Forward, 14.0f, 0.0f}, {Role::Forward, 12.0f, 20.0f}};

// Ponto em coordenadas "de ataque" do time (x positivo = rumo ao gol adversário) para o mundo, e vice-versa.
Vec3 toWorld(const Match& m, int team, float ax, float az) { const float d = m.attackDir(team); return {ax * d, 0.0f, az * d}; }
Vec3 toAttack(const Match& m, int team, const Vec3& w) { const float d = m.attackDir(team); return {w.x * d, w.y, w.z * d}; }
Vec3 goalCenter(const Match& m, int team) { return toWorld(m, team, kHalfLength, 0.0f); }   // gol que o time ataca
Vec3 ownGoal(const Match& m, int team) { return toWorld(m, team, -kHalfLength, 0.0f); }

float effectiveTop(const Player& p) { return p.attr.topSpeed * (0.78f + 0.22f * p.energy); }

// Tempo aproximado para o jogador chegar a um ponto (para escolher quem vai na bola).
float timeTo(const Player& p, const Vec3& target) { return distXZ(p.pos, target) / effectiveTop(p) + 0.15f; }

// ---------------------------------------------------------------------------------------------- chutes e passes

void kick(Match& m, int pi, Vec3 vel, Vec3 spin) {
  Player& p = m.players[size_t(pi)];
  if (m.ball.pos.y < 0.7f) {  // de pé: a bola sai do pé; de cabeça, de onde ela está
    m.ball.pos = p.pos + p.facing * 0.35f;
    m.ball.pos.y = m.ballParams.radius;
  }
  m.ball.vel = vel;
  m.ball.spin = spin;
  m.ball.onGround = vel.y <= 0.01f;
  m.owner = -1;
  m.lastTouch = pi;
  p.cooldown = kKickCooldown;
  p.touchTimer = 0;
  m.actionTick[size_t(pi)] = m.tick;
  m.actionType[size_t(pi)] = vel.y > 2.0f ? Match::ActLob : Match::ActPass;
}

Vec3 rollingSpin(const Vec3& vel, float radius) { return cross(Vec3{0.0f, 1.0f, 0.0f}, flat(vel)) / radius; }

// Velocidade de um passe rasteiro que chega a `to` com ~arrive m/s (desaceleração efetiva ~2,2 m/s² de rolagem + ar).
Vec3 groundPass(const Vec3& from, const Vec3& to, float arrive) {
  const Vec3 d = flat(to - from);
  const float len = length(d);
  const float v0 = clampf(std::sqrt(arrive * arrive + 2.0f * 2.2f * len), 4.0f, 26.0f);
  return normalizeOr(d, Vec3{1, 0, 0}) * v0;
}

// Passe alto/cruzamento: subida de ~30° (tan = 0,577), alcance corrigido pelo arrasto.
Vec3 loftedPass(const Vec3& from, const Vec3& to) {
  const Vec3 d = flat(to - from);
  const float len = std::max(5.0f, length(d));
  const float vh = std::sqrt(len * kG / 1.155f) * 1.1f;
  const Vec3 dir = normalizeOr(d, Vec3{1, 0, 0});
  return {dir.x * vh, vh * 0.577f, dir.z * vh};
}

// Pressão sobre o jogador: 0 (livre) a 1 (adversário colado).
float pressureOn(const Match& m, int pi) {
  const Player& p = m.players[size_t(pi)];
  float best = 99.0f;
  for (const Player& o : m.players) if (o.team != p.team) best = std::min(best, distXZ(o.pos, p.pos));
  return clampf((3.5f - best) / 3.0f, 0.0f, 1.0f);
}

// Segurança de uma linha de passe: menor folga de um adversário até o segmento (descontando quanto ele corre até lá).
float laneSafety(const Match& m, int team, const Vec3& a, const Vec3& b) {
  const Vec3 ab = flat(b - a);
  const float L2 = std::max(dot(ab, ab), 1e-4f);
  float worst = 99.0f;
  for (const Player& o : m.players) {
    if (o.team == team) continue;
    const Vec3 ao = flat(o.pos - a);
    const float t = clampf(dot(ao, ab) / L2, 0.0f, 1.0f);
    const Vec3 closest = a + ab * t;
    // o adversário tem ~0,4 s de reação e corre até a linha enquanto a bola viaja até o ponto
    const float travel = std::sqrt(L2) * t / 14.0f;
    const float slack = distXZ(o.pos, closest) - o.attr.topSpeed * std::max(0.0f, travel - 0.35f) * 0.6f;
    worst = std::min(worst, slack);
  }
  return worst;
}

// Chute (eFootball): a barra dá a velocidade e também o erro — mais barra, mais força e menos precisão; acima de ~80%
// a bola sobe (por cima do gol de longe). Colocado (finesse): mais lento, curva e preciso.
void shoot(Match& m, int pi, float aimZ, float aimY, float power, bool finesse = false) {
  Player& p = m.players[size_t(pi)];
  const int t = p.team;
  const Vec3 goal = goalCenter(m, t);
  const float d = distXZ(p.pos, goal);
  const float pressure = pressureOn(m, pi);
  power = clampf(power, 0.0f, 1.0f);
  const float speed = (15.0f + 15.0f * power + 2.5f * p.attr.shooting) * (finesse ? 0.78f : 1.0f);
  // erro: cresce com a distância, a pressão e a barra; cai com a finalização
  const float sigma = (0.2f + d * 0.03f) * (1.25f - p.attr.shooting) * (1.0f + 0.8f * pressure) * (0.6f + 0.8f * power) *
                      (finesse ? 0.75f : 1.0f);
  Vec3 target = goal;
  target.z += (aimZ + m.rng.gauss() * sigma) * m.attackDir(t);
  const float ty = clampf(aimY + m.rng.gauss() * sigma * 0.5f, 0.15f, 4.5f);
  Vec3 dir = flat(target - p.pos);
  const float len = std::max(1.0f, length(dir));
  dir = dir / len;
  const float time = len / (speed * 0.93f);
  const float vy = (ty - 0.11f) / time + 0.5f * kG * time;
  const float vh = std::sqrt(std::max(1.0f, speed * speed - vy * vy));
  Vec3 vel{dir.x * vh, vy, dir.z * vh};
  // efeito: colocado curva para dentro (spin vertical forte), chute forte quase sem giro
  const float curl = finesse ? 55.0f : 25.0f * (1.0f - power);
  const Vec3 spin{0.0f, (aimZ >= 0.0f ? -1.0f : 1.0f) * m.attackDir(t) * curl, 0.0f};
  kick(m, pi, vel, spin);
  {
    BallState probe = m.ball;
    for (int k = 0; k < 2 * 240 && std::fabs(probe.pos.x) < kHalfLength; ++k) substepBall(probe, m.ballParams);
    m.shotTeam = t;
    m.shotTick = m.tick;
    m.shotCross = probe.pos;
  }
  m.actionType[size_t(pi)] = Match::ActShot;
  TeamStats& st = m.stats[t];
  st.shots++;
  st.xg += clampf(0.42f / (1.0f + (d / 10.0f) * (d / 10.0f)) * (1.0f - 0.5f * pressure), 0.01f, 0.8f);
  if (std::fabs(target.z) < kGoalHalfWidth && ty < kGoalHeight) st.onTarget++;
  m.pendingPasser = -1;
}

// Passe. power < 0: IA (força automática); 0..1: barra do humano — a assistência corrige a direção para o
// companheiro, e a força decide a velocidade (fraco = mais fácil de cortar) e, na enfiada, quanto à frente dele.
void pass(Match& m, int pi, int to, bool through, bool lob, float power = -1.0f) {
  Player& p = m.players[size_t(pi)];
  const Player& r = m.players[size_t(to)];
  const bool human = power >= 0.0f;
  const float pw = human ? clampf(power, 0.0f, 1.0f) : 0.5f;
  Vec3 target = r.pos;
  if (through) {
    // à frente do companheiro, na corrida dele (ou rumo ao gol), sem passar da marca do goleiro
    const Vec3 run = length(flat(r.vel)) > 2.0f ? normalizeOr(flat(r.vel), toWorld(m, p.team, 1.0f, 0.0f)) : toWorld(m, p.team, 1.0f, 0.0f);
    const float lead = human ? 4.0f + 10.0f * pw : 7.0f;
    target = r.pos + run * lead + flat(r.vel) * 0.4f;
    const Vec3 ta = toAttack(m, p.team, target);
    if (ta.x > kHalfLength - 7.0f) target = toWorld(m, p.team, kHalfLength - 7.0f, ta.z);
  } else {
    target = r.pos + flat(r.vel) * 0.5f;
  }
  target.x = clampf(target.x, -kHalfLength + 1.0f, kHalfLength - 1.0f);
  target.z = clampf(target.z, -kHalfWidth + 1.0f, kHalfWidth - 1.0f);
  const float pressure = pressureOn(m, pi);
  const float err = (1.15f - p.attr.passing) * (0.5f + pressure) * distXZ(p.pos, target) * 0.045f * (human ? 0.7f + 0.6f * pw : 1.0f);
  target.x += m.rng.gauss() * err;
  target.z += m.rng.gauss() * err;
  Vec3 vel;
  if (lob) {
    const Vec3 from = flat(p.pos);
    const Vec3 aim = human ? from + flat(target - from) * (0.88f + 0.24f * pw) : target;
    vel = loftedPass(p.pos, aim);
  } else {
    vel = groundPass(p.pos, target, human ? 3.0f + 11.0f * pw : (through ? 9.0f : 8.0f));
  }
  kick(m, pi, vel, lob ? Vec3{} : rollingSpin(vel, m.ballParams.radius));
  m.stats[p.team].passes++;
  m.pendingPasser = pi;
  m.passTarget = to;
}

// Melhor companheiro numa direção (assistência de passe: ângulo, distância, linha livre, avanço).
int choosePassTarget(const Match& m, int pi, Vec3 dir, bool through, float* outScore = nullptr) {
  const Player& p = m.players[size_t(pi)];
  int best = -1;
  float bestScore = -1e9f;
  const bool useDir = length(dir) > 0.1f;
  dir = normalizeOr(flat(dir), Vec3{});
  for (const Player& r : m.players) {
    if (r.team != p.team || r.index == pi || r.role == Role::Goalkeeper) continue;
    const Vec3 to = flat(r.pos - p.pos);
    const float d = length(to);
    if (d < 3.0f || d > 50.0f) continue;
    const Vec3 toN = to / d;
    const float progress = toAttack(m, p.team, r.pos).x - toAttack(m, p.team, p.pos).x;
    const float safety = laneSafety(m, p.team, p.pos, r.pos);
    float space = 99.0f;
    for (const Player& o : m.players) if (o.team != p.team) space = std::min(space, distXZ(o.pos, r.pos));
    float score = progress * (through ? 0.1f : 0.06f) + clampf(safety, -3.0f, 4.0f) * 0.45f - (safety < 0.8f ? 3.0f : 0.0f) + clampf(space, 0.0f, 7.0f) * 0.18f -
                  std::max(0.0f, d - 30.0f) * 0.08f;
    // cone de ±40° em volta do joystick (assistência do eFootball): fora dele só se não houver ninguém dentro
    if (useDir) score += dot(toN, dir) * 5.0f - (dot(toN, dir) < 0.766f ? 8.0f : 0.0f);
    if (score > bestScore) { bestScore = score; best = r.index; }
  }
  if (outScore) *outScore = bestScore;
  return best;
}

// ---------------------------------------------------------------------------------------------- IA

// Alvo de posicionamento sem a bola (bloco do time + forma + deslocamento para o lado da bola).
Vec3 shapeTarget(const Match& m, const Player& p, bool inPossession) {
  const Vec3 b = toAttack(m, p.team, m.ball.pos);
  if (p.role == Role::Goalkeeper) {
    const Vec3 g{-kHalfLength, 0.0f, 0.0f};
    const Vec3 tob = flat(b - g);
    const float d = length(tob);
    const float depth = clampf(d * 0.1f, 1.0f, inPossession ? 12.0f : 5.0f);
    const Vec3 a = g + normalizeOr(tob, Vec3{1, 0, 0}) * depth;
    return toWorld(m, p.team, a.x, clampf(a.z, -kGoalHalfWidth, kGoalHalfWidth));
  }
  const Shape& sh = kShape[p.index % kPlayersPerTeam];
  const float center = clampf(b.x * 0.65f + (inPossession ? 8.0f : -9.0f), -34.0f, 32.0f);
  const float lengthScale = inPossession ? 1.35f : 0.85f, widthScale = inPossession ? 1.4f : 0.95f;
  float ax = center + sh.sx * lengthScale;
  float az = sh.sz * widthScale + b.z * (inPossession ? 0.15f : 0.35f);
  if (p.role == Role::Defender) ax = std::max(ax, -42.0f);
  if (p.role == Role::Forward && inPossession) {
    ax = std::min(ax + 4.0f, 44.0f);
    if (b.x > 25.0f) { ax = std::max(ax, 40.0f); az *= 0.45f; }  // atacar a área
  }
  ax = clampf(ax, -kHalfLength + 3.0f, kHalfLength - 3.0f);
  az = clampf(az, -kHalfWidth + 1.5f, kHalfWidth - 1.5f);
  return toWorld(m, p.team, ax, az);
}

// Onde ir buscar a bola solta: ponto previsto um pouco à frente da bola.
Vec3 interceptPoint(const Match& m, const Player& p) {
  const float t = clampf(timeTo(p, m.ball.pos) * 0.6f, 0.0f, 1.2f);
  Vec3 at = m.ball.pos + flat(m.ball.vel) * t;
  at.x = clampf(at.x, -kHalfLength - 1.0f, kHalfLength + 1.0f);
  at.z = clampf(at.z, -kHalfWidth - 1.0f, kHalfWidth + 1.0f);
  return flat(at);
}

// Onde receber a bola que vem rolando: o ponto da trajetória (próximos ~2 s) que o jogador alcança primeiro.
Vec3 receivePoint(const Match& m, const Player& p) {
  Vec3 pos = flat(m.ball.pos), vel = flat(m.ball.vel);
  for (int k = 1; k <= 20; ++k) {
    const float t = 0.1f * float(k);
    const float sp = std::max(0.0f, length(vel) - 2.2f * t);
    const Vec3 at = pos + normalizeOr(vel, Vec3{}) * (length(vel) * t - 1.1f * t * t * (sp > 0.0f ? 1.0f : 0.0f));
    if (timeTo(p, at) <= t || sp <= 0.0f) return at;
  }
  return pos + vel * 2.0f;
}

struct Intent {
  Vec3 target{};
  bool sprint = false;
  bool faceBall = true;
};

// Decisão do portador da bola (IA): chutar, passar ou conduzir. Devolve a direção de condução.
Vec3 carrierDecision(Match& m, int pi) {
  Player& p = m.players[size_t(pi)];
  const int t = p.team;
  const Vec3 goal = goalCenter(m, t);
  const float d = distXZ(p.pos, goal);
  const Vec3 a = toAttack(m, t, p.pos);
  const float pressure = pressureOn(m, pi);
  // chute
  if (d < 32.0f && std::fabs(a.z) < 24.0f) {
    const float open = clampf(laneSafety(m, t, p.pos, goal) / 2.0f, 0.0f, 1.0f);
    const float xg = 0.42f / (1.0f + (d / 10.0f) * (d / 10.0f)) * (0.4f + 0.6f * open);
    if (xg > 0.012f + 0.018f * m.rng.uniform() || d < 18.0f) {
      const float side = (a.z >= 0.0f ? -1.0f : 1.0f);  // canto oposto
      shoot(m, pi, side * (2.2f + m.rng.uniform() * 1.0f), 0.5f + m.rng.uniform() * 1.3f, 0.55f + 0.4f * m.rng.uniform());
      return {};
    }
  }
  // cruzamento: pela ponta no último terço, com companheiro na área
  if (a.x > 30.0f && std::fabs(a.z) > 14.0f) {
    int inBox = 0;
    for (const Player& q : m.players)
      if (q.team == t && q.index != pi && q.role == Role::Forward) {
        const Vec3 qa = toAttack(m, t, q.pos);
        if (qa.x > 36.0f && std::fabs(qa.z) < 12.0f) ++inBox;
      }
    if (inBox > 0 && m.rng.uniform() < 0.5f) {
      const Vec3 box = toWorld(m, t, kHalfLength - 8.0f - 4.0f * m.rng.uniform(), m.rng.signedUnit() * 5.0f);
      kick(m, pi, loftedPass(p.pos, box), {});
      m.stats[t].passes++;
      m.pendingPasser = pi;
      return {};
    }
  }
  // passe
  float passScore = -1e9f, throughScore = -1e9f;
  const int to = choosePassTarget(m, pi, {}, false, &passScore);
  const int toThrough = choosePassTarget(m, pi, toWorld(m, t, 1.0f, 0.0f), true, &throughScore);
  // condução: espaço à frente
  float space = 99.0f;
  for (const Player& o : m.players) {
    if (o.team == t) continue;
    const Vec3 rel = toAttack(m, t, o.pos) - a;
    if (rel.x > -1.0f && std::fabs(rel.z) < 4.0f + rel.x * 0.4f) space = std::min(space, length(flat(rel)));
  }
  float dribbleScore = clampf(space, 0.0f, 12.0f) * 0.32f + p.attr.dribbling * 1.5f - pressure * 2.5f +
                       (a.x > 30.0f ? 1.0f : 0.0f) + (a.x > 15.0f ? 1.5f : 0.0f);  // no terço final, encara mais
  if (p.touchTimer >= 0 && m.possessionAge < 20 + int(m.rng.uniform() * 25.0f) && pressure < 0.4f) dribbleScore += 3.0f;
  const float noise = m.rng.signedUnit() * 0.4f;
  const float best = std::max(passScore, throughScore);
  if (best + noise > dribbleScore && to >= 0) {
    const bool useThrough = throughScore > passScore && toThrough >= 0 &&
                            m.players[size_t(toThrough)].role != Role::Defender;
    const int r = useThrough ? toThrough : to;
    const bool lob = !useThrough && laneSafety(m, t, p.pos, m.players[size_t(r)].pos) < 0.0f &&
                     distXZ(p.pos, m.players[size_t(r)].pos) > 18.0f;
    pass(m, pi, r, useThrough, lob);
    return {};
  }
  // conduz para o gol, desviando do adversário mais próximo
  Vec3 dir = normalizeOr(flat(goal - p.pos), Vec3{1, 0, 0});
  for (const Player& o : m.players) {
    if (o.team == t) continue;
    const Vec3 off = flat(p.pos - o.pos);
    const float dd = length(off);
    if (dd < 4.0f && dd > 0.01f) dir += off / dd * ((4.0f - dd) * 0.35f);
  }
  return normalizeOr(dir, Vec3{1, 0, 0});
}

// ---------------------------------------------------------------------------------------------- movimento

// Seno e cosseno de ângulos pequenos (|x| < 0,6 rad) por série de Taylor: só soma e multiplicação, o mesmo
// resultado em qualquer aparelho (a libm não garante isso).
void sinCosSmall(float x, float& s, float& c) {
  const float x2 = x * x;
  s = x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
  c = 1.0f - x2 / 2.0f * (1.0f - x2 / 12.0f * (1.0f - x2 / 30.0f));
}

// Gira a direção unitária `a` rumo a `b` no máximo `maxAngle` radianos (no plano xz).
Vec3 rotateToward(const Vec3& a, const Vec3& b, float maxAngle) {
  float s, c;
  sinCosSmall(std::min(maxAngle, 0.6f), s, c);
  const float d = dot(a, b);
  if (d >= c) return b;
  Vec3 perp = b - a * d;
  if (dot(perp, perp) < 1e-8f) perp = Vec3{-a.z, 0.0f, a.x};  // b oposto: gira para a esquerda
  perp = normalizeOr(perp, Vec3{-a.z, 0.0f, a.x});
  return normalizeOr(a * c + perp * s, b);
}

// Taxa de giro (eFootball: fixa para todos, cai com a velocidade): 720°/s parado, 360°/s a 5 m/s, 180°/s a 8 m/s.
float turnRate(float speed) {
  constexpr float kPi = 3.14159265f;
  if (speed < 5.0f) return 4.0f * kPi - 2.0f * kPi * (speed / 5.0f);
  return std::max(kPi, 2.0f * kPi - kPi * ((speed - 5.0f) / 3.0f));
}

// Locomoção do eFootball (efootballexplained, Speed/Acc 80): aceleração em 3 fases — 10 m/s² até 15 km/h, 5 m/s² até
// 22,5 km/h, 1,3 m/s² até a máxima (~2,6 s de 0 à máxima) —, frenagem de 9 m/s² e a direção da corrida limitada pela
// taxa de giro (raio de curva; em alta velocidade, mudar de direção força frear).
void movePlayer(Player& p, const Vec3& desiredVel) {
  const Vec3 v = flat(p.vel);
  float sp = length(v);
  const float want = length(desiredVel);
  const Vec3 wantDir = want > 0.05f ? desiredVel / want : (sp > 0.05f ? v / sp : p.facing);
  Vec3 dir = sp > 0.05f ? v / sp : wantDir;
  if (sp < 0.6f) dir = wantDir;  // quase parado: sai na direção pedida
  else dir = rotateToward(dir, wantDir, turnRate(sp) * kTick);
  // velocidade-alvo: a pedida, menos o que não está alinhado (virando, freia até apontar para lá)
  const float align = clampf(dot(dir, wantDir), 0.0f, 1.0f);
  const float target = want * align;
  const float k = std::max(p.attr.accel, 1.0f) / 5.5f;
  const float accel = (sp < 4.17f ? 10.0f : sp < 6.25f ? 5.0f : 1.3f) * k;
  if (target > sp) sp = std::min(target, sp + accel * kTick);
  else sp = std::max(target, sp - 9.0f * kTick);
  p.vel = dir * sp;
  p.pos += p.vel * kTick;
  p.pos.y = 0.0f;
  // cansaço: corrida acima de 75% da velocidade máxima gasta; abaixo, recupera
  const float load = sp / p.attr.topSpeed;
  p.energy = clampf(p.energy + (load > 0.75f ? -0.0009f * (1.3f - p.attr.stamina) : 0.0004f) , 0.3f, 1.0f);
}

void turnTo(Player& p, const Vec3& dir, float rate) {
  (void)rate;
  const Vec3 d = normalizeOr(flat(dir), p.facing);
  p.facing = rotateToward(p.facing, d, turnRate(length(flat(p.vel))) * kTick);
}

Vec3 arriveVel(const Player& p, const Vec3& target, bool sprint) {
  const Vec3 to = flat(target - p.pos);
  const float d = length(to);
  if (d < 0.25f) return {};
  const float top = effectiveTop(p) * (sprint ? 1.0f : 0.7f);  // IA sem bola: 70% da máxima (Konami v5.0)
  const float speed = std::min(top, d * 2.2f);  // chega desacelerando
  return to / d * speed;
}

// ---------------------------------------------------------------------------------------------- regras

void placeForKickoff(Match& m, int kickoffTeam) {
  for (Player& p : m.players) {
    const Shape& sh = kShape[p.index % kPlayersPerTeam];
    float ax = p.role == Role::Goalkeeper ? -kHalfLength + 1.0f : clampf(-26.0f + sh.sx * 1.4f, -45.0f, -2.0f);
    float az = sh.sz;
    if (p.team == kickoffTeam && p.index % kPlayersPerTeam == 9) { ax = -0.4f; az = 0.0f; }
    if (p.team == kickoffTeam && p.index % kPlayersPerTeam == 6) { ax = -8.0f; az = 0.0f; }
    p.pos = toWorld(m, p.team, ax, az);
    p.vel = {};
    p.facing = toWorld(m, p.team, 1.0f, 0.0f);
    p.cooldown = 0;
  }
  m.ball = BallState{};
  m.owner = kickoffTeam * kPlayersPerTeam + 9;
  m.lastTouch = m.owner;
  m.restartTeam = kickoffTeam;
  m.phase = Phase::Kickoff;
  m.phaseTicks = 0;
}

void startRestart(Match& m, Phase ph, int team, Vec3 spot) {
  if (m.pendingPasser >= 0) m.stats[teamOf(m.pendingPasser)].passesOut++;
  m.phase = ph;
  m.phaseTicks = 0;
  m.restartTeam = team;
  m.owner = -1;
  m.ball = BallState{};
  m.ball.pos = {spot.x, m.ballParams.radius, spot.z};
  m.pendingPasser = -1;
  m.passTarget = -1;
}

int nearestOfTeam(const Match& m, int team, const Vec3& at, bool includeKeeper) {
  int best = -1;
  float bd = 1e9f;
  for (const Player& p : m.players) {
    if (p.team != team || (!includeKeeper && p.role == Role::Goalkeeper)) continue;
    const float d = dist2(p.pos, at);
    if (d < bd) { bd = d; best = p.index; }
  }
  return best;
}

void checkBallOut(Match& m) {
  const Vec3 b = m.ball.pos;
  const int lt = m.lastTouch >= 0 ? teamOf(m.lastTouch) : 0;
  if (std::fabs(b.x) > kHalfLength + m.ballParams.radius) {
    const float side = b.x > 0.0f ? 1.0f : -1.0f;
    const int attacker = m.attackDir(0) == side ? 0 : 1;  // time que ataca este gol
    if (std::fabs(b.z) < kGoalHalfWidth && b.y < kGoalHeight) {
      m.score[attacker]++;
      m.phase = Phase::Goal;
      m.phaseTicks = 0;
      m.restartTeam = other(attacker);
      m.owner = -1;
      m.pendingPasser = -1;
      return;
    }
    if (lt == other(attacker)) {
      m.stats[attacker].corners++;
      startRestart(m, Phase::Corner, attacker, {side * (kHalfLength - 0.5f), 0.0f, b.z > 0.0f ? kHalfWidth - 0.5f : -kHalfWidth + 0.5f});
    } else {
      startRestart(m, Phase::GoalKick, other(attacker), {side * (kHalfLength - 5.5f), 0.0f, 0.0f});
    }
    return;
  }
  if (std::fabs(b.z) > kHalfWidth + m.ballParams.radius) {
    startRestart(m, Phase::ThrowIn, other(lt), {clampf(b.x, -kHalfLength + 1.0f, kHalfLength - 1.0f), 0.0f, b.z > 0.0f ? kHalfWidth - 0.3f : -kHalfWidth + 0.3f});
  }
}

// Goleiro: quando a bola cruza a linha em que ele está, vindo para o gol, ele mergulha. Alcance lateral pelo
// atributo de goleiro, altura até o travessão; a chance de defesa cai com a distância do mergulho e a força do chute.
void keeperSaves(Match& m) {
  if (m.phase != Phase::Play || m.owner >= 0) return;
  const float sp = length(m.ball.vel);
  if (sp < 5.0f) return;
  for (int t = 0; t < 2; ++t) {
    Player& gk = m.players[size_t(t * kPlayersPerTeam)];
    if (gk.cooldown > 0) continue;
    const float dir = m.attackDir(t);  // o gol dele fica em x = -dir * 52,5
    const float ballDepth = (m.ball.pos.x - gk.pos.x) * dir;   // > 0: bola ainda à frente do goleiro
    const float vx = m.ball.vel.x * dir;
    if (vx >= -1.0f || ballDepth > 0.6f || ballDepth < -0.9f) continue;  // não vem para o gol ou já passou
    const float lateral = std::fabs(m.ball.pos.z - gk.pos.z);
    const float reach = 2.6f + 0.5f * gk.attr.keeping;  // mergulho ~2,8 m (×0,94→×1,12 pelo alcance)
    if (lateral > reach || m.ball.pos.y > 2.7f) continue;
    gk.cooldown = 12;
    const float stretch = lateral / reach;
    const float pSave = clampf(0.25f + 0.85f * gk.attr.keeping - stretch * 0.45f - std::max(0.0f, sp - 22.0f) * 0.03f -
                                   (m.ball.pos.y > 2.0f ? 0.15f : 0.0f), 0.05f, 0.95f);
    if (m.rng.uniform() >= pSave) continue;
    m.stats[t].saves++;
    m.lastTouch = gk.index;
    m.actionTick[size_t(gk.index)] = m.tick;
    m.actionType[size_t(gk.index)] = Match::ActSave;
    m.shotTeam = -1;
    // segura chutes até 92→110 km/h (pegada), não logo depois do chute (0,37→0,22 s); senão espalma
    const float sinceShot = float(m.tick - m.shotTick) * kTick;
    if (sp < 25.5f + 5.0f * gk.attr.keeping && stretch < 0.6f && sinceShot > 0.37f - 0.15f * gk.attr.keeping) {  // segura
      m.owner = gk.index;
      m.ball.vel = {};
      m.ball.spin = {};
    } else {  // espalma para fora, para o lado do mergulho
      const float side = m.ball.pos.z >= gk.pos.z ? 1.0f : -1.0f;
      m.ball.vel = {dir * sp * 0.15f, 2.0f + m.rng.uniform() * 3.0f, side * (4.0f + sp * 0.2f)};
      m.ball.onGround = false;
    }
    m.pendingPasser = -1;
  }
}

// Chute do humano: canto pelo joystick (para cima/baixo na tela = lado do gol); sem direção, o canto longe do
// chutador (assistência). Altura pela barra: até ~85% baixo/meia altura, acima disso sobe (cheia: por cima com frequência).
void humanShoot(Match& m, int pi, const Vec3& stick, float power, bool finesse) {
  const Player& p = m.players[size_t(pi)];
  const int t = p.team;
  const Vec3 a = toAttack(m, t, p.pos);
  float aimZ;
  if (std::fabs(stick.z) > 0.25f) aimZ = clampf(stick.z * m.attackDir(t), -1.0f, 1.0f) * 2.9f;
  else aimZ = (a.z >= 0.0f ? -1.0f : 1.0f) * 2.6f;
  float aimY = finesse ? 0.6f + 0.7f * power : 0.35f + 1.1f * power + std::max(0.0f, power - 0.85f) * 9.0f;
  shoot(m, pi, aimZ, aimY, power, finesse);
}

// Quem domina a bola: o mais perto que alcança, se ela estiver baixa e não rápida demais.
void resolvePossession(Match& m) {
  if (m.phase == Phase::Kickoff && m.owner >= 0) return;  // saída de bola: só o toque do batedor libera a bola
  if (m.owner >= 0) {
    // em arrancada a bola vai até ~3 m à frente; passou disso (ou subiu), ninguém a domina
    if (distXZ(m.players[size_t(m.owner)].pos, m.ball.pos) > 4.5f || m.ball.pos.y > 1.2f) m.owner = -1;
  }
  if (m.ball.pos.y > 0.7f) return;
  int best = -1;
  float bd = 1e9f;
  // Interceptação contínua: a bola a 30 m/s anda 1 m por passo; mede a menor distância do pé ao trecho que ela
  // percorreu neste passo (antes, um passe forte atravessava as pernas de quem estava no caminho).
  const Vec3 seg = flat(m.ball.pos - m.prevBallPos);
  const float segL2 = dot(seg, seg);
  for (const Player& p : m.players) {
    if (p.cooldown > 0 || p.index == m.owner) continue;
    const Vec3 foot = p.pos + p.facing * 0.3f;
    const float reach = (p.role == Role::Goalkeeper ? 1.0f : 0.7f) + 0.2f * p.attr.dribbling;
    float d = distXZ(foot, m.ball.pos);
    if (m.owner < 0 && segL2 > 0.04f) {
      const float u = clampf(dot(flat(foot - m.prevBallPos), seg) / segL2, 0.0f, 1.0f);
      d = std::min(d, distXZ(foot, m.prevBallPos + seg * u));
    }
    if (d > reach || d > bd) continue;
    // roubo: só se o portador não estiver com a bola colada
    if (m.owner >= 0) {
      const Player& o = m.players[size_t(m.owner)];
      if (o.team == p.team) continue;
      // roubo (eFootball, desarme geométrico): só com a bola longe do pé do condutor — de 1,2 m (marcador fraco) a
      // 0,2 m (marcador forte) — e o marcador mais perto dela que o condutor
      if (distXZ(o.pos + o.facing * 0.45f, m.ball.pos) < 1.2f - 1.0f * p.attr.defending) continue;
      if (d > distXZ(o.pos + o.facing * 0.45f, m.ball.pos)) continue;
    }
    bd = d;
    best = p.index;
  }
  if (best < 0) return;
  Player& p = m.players[size_t(best)];
  const float rel = length(flat(m.ball.vel - p.vel));
  if (rel > 19.0f && p.role != Role::Goalkeeper) {  // bola forte demais: desvia no corpo
    m.ball.vel = m.ball.vel * 0.35f + p.facing * 2.0f;
    m.lastTouch = best;
    p.cooldown = 6;
    m.pendingPasser = -1;
    return;
  }
  const int prev = m.owner >= 0 ? m.owner : m.lastTouch;
  if (prev >= 0 && teamOf(prev) != p.team && m.owner >= 0) m.stats[p.team].tackles++;
  if (m.pendingPasser >= 0) {
    if (teamOf(m.pendingPasser) == p.team && m.pendingPasser != best) m.stats[p.team].passesCompleted++;
    else if (teamOf(m.pendingPasser) != p.team) m.stats[teamOf(m.pendingPasser)].passesIntercepted++;
    m.pendingPasser = -1;
  }
  m.owner = best;
  m.lastTouch = best;
  m.passTarget = -1;
  m.possessionAge = 0;
  m.shotTeam = -1;
  p.touchTimer = 0;
  // Primeiro toque (domínio automático): amortece a bola na frente do corpo; o erro cresce com a velocidade da bola
  // e a pressão e cai com o controle. O humano orienta o toque pelo joystick (com Acelerar, um toque longo).
  const float err = clampf((rel - 6.0f) * 0.04f * (1.2f - p.attr.dribbling) * (1.0f + pressureOn(m, best)), 0.0f, 1.2f);
  const Vec3 jitter{m.rng.signedUnit() * err * 2.0f, 0.0f, m.rng.signedUnit() * err * 2.0f};
  const int t = p.team;
  if (m.controlled[t] == best && length(m.humanStick[t]) > 0.3f) {
    const Vec3 sd = normalizeOr(m.humanStick[t], p.facing);
    p.facing = sd;
    m.ball.vel = flat(p.vel) * 0.6f + sd * (m.humanDash[t] ? 4.5f : 2.0f) + jitter;
  } else {
    m.ball.vel = flat(p.vel) * 1.05f + p.facing * (0.8f + err * 4.0f) + jitter;
  }
  m.ball.spin = rollingSpin(m.ball.vel, m.ballParams.radius);
  m.ball.onGround = true;
  // passe/chute de primeira comandado antes de a bola chegar
  if (m.controlled[t] == best && m.queued[t]) {
    const uint8_t q = m.queued[t];
    m.queued[t] = 0;
    m.ball.pos = p.pos + p.facing * 0.35f;
    m.ball.pos.y = m.ballParams.radius;
    if (q & Input::Shoot) {
      p.facing = normalizeOr(flat(goalCenter(m, t) - p.pos), p.facing);
      humanShoot(m, best, m.queuedDir[t], m.queuedPower[t], m.queuedSpecial[t]);
    } else {
      const int to = choosePassTarget(m, best, length(m.queuedDir[t]) > 0.2f ? m.queuedDir[t] : p.facing, q & Input::Through);
      if (to >= 0) pass(m, best, to, q & Input::Through, m.queuedLob[t], m.queuedPower[t]);
    }
  }
}

// Condução por toques (eFootball): a bola nunca fica presa ao jogador — rola livre entre os toques e o condutor
// corre atrás dela. Um toque só sai passado o intervalo mínimo E com a bola ao alcance do pé, à frente do quadril;
// o joystick (ou a IA) só escolhe a direção e a força do próximo toque. Intervalos (efootballexplained): 0,5 s
// conduzindo reto abaixo de 20 km/h; 0,49→0,40 s em viradas de 45° ou mais; senão 0,23→0,18 s (Tight Possession).
// O toque empurra a bola o bastante para ela ficar ~0,7 m à frente do pé no próximo toque andando e ~2 m em
// arrancada (0,6x numa virada forte em velocidade).
void carrierStep(Match& m, Player& p, Vec3 desiredDir, float desiredSpeed, bool dash) {
  desiredDir = normalizeOr(flat(desiredDir), p.facing);
  const float sp = length(flat(p.vel));
  const float drib = p.attr.dribbling;
  const Vec3 ballFlat = flat(m.ball.pos), bv = flat(m.ball.vel);
  const Vec3 contact = p.pos + p.facing * 0.45f;
  const float elapsed = float(m.tick - p.lastTouchTick) * kTick;
  const float cosTurn = dot(p.facing, desiredDir);
  float tmin;
  if (desiredSpeed < 0.3f) tmin = 0.3f;
  else if (sp < 5.56f && cosTurn > 0.924f) tmin = 0.5f;
  else if (cosTurn < 0.707f) tmin = 0.49f - 0.09f * drib;
  else tmin = 0.23f - 0.05f * drib;
  const Vec3 toBall = ballFlat - p.pos;
  const bool inFront = dot(normalizeOr(toBall, p.facing), p.facing) > -0.17f;  // até ~100° do corpo
  const bool canTouch = elapsed >= tmin && distXZ(contact, ballFlat) <= 0.7f && m.ball.pos.y < 0.35f && inFront;
  if (canTouch) {
    if (desiredSpeed < 0.3f) {
      m.ball.vel = flat(p.vel) * 0.25f;  // amortece: a bola fica no pé
    } else {
      // a bola sai mais rápida que o condutor o bastante para ficar `lead` à frente no próximo toque
      const float base = dash ? std::max(sp, desiredSpeed * 0.6f) : desiredSpeed;
      const float lead = dash ? 1.4f : 0.45f, period = dash ? 0.7f : 0.5f;
      float push = std::min(base + lead / period, desiredSpeed * 1.6f + 1.0f);
      if (cosTurn < 0.0f && sp > 5.0f) push *= 0.6f;  // virada forte em velocidade: toque curto
      const Vec3 noise{m.rng.signedUnit() * 0.035f, 0.0f, m.rng.signedUnit() * 0.035f};  // ~±2°
      m.ball.vel = normalizeOr(desiredDir + noise, desiredDir) * push;
      m.actionTick[size_t(p.index)] = m.tick;
      m.actionType[size_t(p.index)] = Match::ActTouch;
    }
    m.ball.spin = rollingSpin(m.ball.vel, m.ballParams.radius);
    m.ball.onGround = true;
    p.lastTouchTick = m.tick;
  }
  // persegue o ponto onde a bola vai estar no próximo toque permitido (desaceleração de rolagem ~2 m/s²)
  const float remaining = std::max(tmin - float(m.tick - p.lastTouchTick) * kTick, 0.15f);
  const float bs = length(bv);
  const float travel = std::max(0.0f, bs * remaining - 1.0f * remaining * remaining);
  const Vec3 pred = ballFlat + normalizeOr(bv, Vec3{}) * std::min(travel, bs * bs / 4.0f);
  const Vec3 aim = pred - normalizeOr(pred - p.pos, desiredDir) * 0.45f;  // o pé (não o corpo) chega na bola
  const Vec3 toAim = flat(aim - p.pos);
  const float dAim = length(toAim);
  const float cap = dash ? effectiveTop(p) * 0.92f : 6.5f;  // folga para alcançar a bola entre os toques
  float speed = std::min(dAim / remaining, cap);
  if (dAim < 0.15f) speed = 0.0f;
  const Vec3 chase = dAim > 0.05f ? toAim / dAim : desiredDir;
  movePlayer(p, chase * speed);
  turnTo(p, dAim > 0.3f ? chase : desiredDir, 1.0f);
}

}  // namespace

float Match::gameMinute() const {
  const float halfTicks = cfg.halfSeconds * float(kTicksPerSecond);
  return (half == 1 ? 0.0f : 45.0f) + 45.0f * float(halfTick) / halfTicks;
}

void initMatch(Match& m, const MatchConfig& cfg) {
  m = Match{};
  m.cfg = cfg;
  m.rng.s = cfg.seed * 0x9E3779B97F4A7C15ull + 0x2545F4914F6CDD1Dull;
  for (int i = 0; i < 2 * kPlayersPerTeam; ++i) {
    Player& p = m.players[size_t(i)];
    p.team = i / kPlayersPerTeam;
    p.index = i;
    p.role = kShape[i % kPlayersPerTeam].role;
    p.attr = cfg.teamAttr[p.team];
    // variação individual em torno do time (±8%), e o goleiro sabe defender
    auto vary = [&](float v) { return clampf(v * (1.0f + 0.08f * m.rng.signedUnit()), 0.05f, 0.99f); };
    p.attr.passing = vary(p.attr.passing); p.attr.shooting = vary(p.attr.shooting);
    p.attr.dribbling = vary(p.attr.dribbling); p.attr.defending = vary(p.attr.defending);
    p.attr.topSpeed *= 1.0f + 0.05f * m.rng.signedUnit();
    if (p.role == Role::Goalkeeper) { p.attr.keeping = std::max(p.attr.keeping, 0.65f); p.attr.topSpeed *= 0.85f; }
    if (p.role == Role::Forward) p.attr.shooting = clampf(p.attr.shooting + 0.1f, 0.0f, 0.99f);
    if (p.role == Role::Defender) p.attr.defending = clampf(p.attr.defending + 0.1f, 0.0f, 0.99f);
  }
  placeForKickoff(m, 0);
}

bool finished(const Match& m) { return m.phase == Phase::FullTime; }

void stepMatch(Match& m, const Input* inputs) {
  if (m.phase == Phase::FullTime) return;
  ++m.tick;
  ++m.phaseTicks;
  // relógio do tempo (corre sempre; bola parada vira acréscimo)
  ++m.halfTick;
  if (m.phase != Phase::Play && m.phase != Phase::HalfTime) m.stoppageTicks++;
  const int halfEnd = int(m.cfg.halfSeconds * float(kTicksPerSecond)) + std::min(m.stoppageTicks / 3, 30 * kTicksPerSecond / 2);
  if (m.phase == Phase::HalfTime) {
    if (m.phaseTicks > 2 * kTicksPerSecond) {
      m.half = 2; m.halfTick = 0; m.stoppageTicks = 0;
      placeForKickoff(m, 1);
    }
    return;
  }
  if (m.halfTick >= halfEnd && (m.phase == Phase::Play || m.phase == Phase::Kickoff)) {
    if (m.half == 1) { m.phase = Phase::HalfTime; m.phaseTicks = 0; m.owner = -1; return; }
    m.phase = Phase::FullTime;
    return;
  }

  for (Player& p : m.players) if (p.cooldown > 0) --p.cooldown;

  // ---- bola parada ----
  if (m.phase == Phase::Goal) {
    if (m.phaseTicks > 3 * kTicksPerSecond) placeForKickoff(m, m.restartTeam);
    return;
  }
  const bool restart = m.phase == Phase::ThrowIn || m.phase == Phase::Corner || m.phase == Phase::GoalKick;
  if (restart && m.owner < 0) {
    const int taker = m.phase == Phase::GoalKick ? m.restartTeam * kPlayersPerTeam : nearestOfTeam(m, m.restartTeam, m.ball.pos, false);
    Player& tp = m.players[size_t(taker)];
    if (distXZ(tp.pos, m.ball.pos) < 1.2f || m.phaseTicks > 4 * kTicksPerSecond) {
      tp.pos = m.ball.pos - toWorld(m, tp.team, 0.6f, 0.0f);
      tp.pos.y = 0.0f;
      tp.vel = {};
      tp.facing = normalizeOr(flat(goalCenter(m, tp.team) - tp.pos), Vec3{1, 0, 0});
      m.owner = taker;
      m.lastTouch = taker;
      m.phaseTicks = 0;
    }
  }

  // ---- controle humano: quem é o jogador controlado ----
  // Com a bola: o portador. Passe a caminho: o recebedor (o controle passa para ele, como no eFootball). Na defesa,
  // troca assistida: o jogador que chega primeiro na bola, com folga de 0,35 s e espera de 0,5 s entre trocas (não
  // pula de um para outro); Trocar escolhe o melhor que não é o atual.
  for (int t = 0; t < 2; ++t) {
    if (m.cfg.humanTeam != t) { m.controlled[t] = -1; continue; }
    const Input& in = inputs ? inputs[t] : Input{};
    if (m.switchCooldown[t] > 0) --m.switchCooldown[t];
    if (m.owner >= 0 && teamOf(m.owner) == t) { m.controlled[t] = m.owner; continue; }
    if (m.owner < 0 && m.passTarget >= 0 && teamOf(m.passTarget) == t) { m.controlled[t] = m.passTarget; continue; }
    const bool switchPressed = (in.buttons & Input::Switch) && !(m.prevButtons[t] & Input::Switch);
    int best = -1, second = -1;
    float bt = 1e9f, st = 1e9f;
    for (const Player& q : m.players) {
      if (q.team != t || q.role == Role::Goalkeeper) continue;
      const float tt = timeTo(q, interceptPoint(m, q));
      if (tt < bt) { st = bt; second = best; bt = tt; best = q.index; }
      else if (tt < st) { st = tt; second = q.index; }
    }
    if (best < 0) continue;
    int& c = m.controlled[t];
    if (c < 0) { c = best; continue; }
    if (switchPressed) {
      c = best != c ? best : (second >= 0 ? second : best);
      m.switchCooldown[t] = 15;
      continue;
    }
    const float ct = timeTo(m.players[size_t(c)], interceptPoint(m, m.players[size_t(c)]));
    if (best != c && m.switchCooldown[t] == 0 && bt + 0.35f < ct) {
      c = best;
      m.switchCooldown[t] = 15;
    }
  }

  // ---- intenções ----
  const int ownerTeam = m.owner >= 0 ? teamOf(m.owner) : (m.lastTouch >= 0 ? teamOf(m.lastTouch) : -1);
  // quem vai na bola em cada time (o mais rápido até ela)
  int chaser[2] = {-1, -1}, cover[2] = {-1, -1};
  for (int t = 0; t < 2; ++t) {
    float b1 = 1e9f, b2 = 1e9f;
    for (const Player& p : m.players) {
      if (p.team != t || p.role == Role::Goalkeeper || p.index == m.owner) continue;
      const float tt = timeTo(p, m.ball.pos);
      if (tt < b1) { b2 = b1; cover[t] = chaser[t]; b1 = tt; chaser[t] = p.index; }
      else if (tt < b2) { b2 = tt; cover[t] = p.index; }
    }
  }
  const bool live = m.phase == Phase::Play || (m.phase == Phase::Kickoff && m.owner >= 0) || (restart && m.owner >= 0);

  for (Player& p : m.players) {
    const int t = p.team;
    Intent it;
    const bool isOwner = p.index == m.owner;
    const bool human = m.controlled[t] == p.index;
    if (human) continue;  // tratado abaixo
    if (isOwner) {
      if (!live) { movePlayer(p, {}); continue; }
      const bool waitRestart = (m.phase == Phase::Kickoff || restart) && m.phaseTicks < kRestartPause;
      if (waitRestart) { movePlayer(p, {}); continue; }
      Vec3 dir;
      if (--m.aiThinkTimer[t] <= 0 || m.phase != Phase::Play) {
        m.aiThinkTimer[t] = 6;
        if (m.phase == Phase::Corner) {  // cruzamento na área
          const Vec3 box = toWorld(m, t, kHalfLength - 9.0f, m.rng.signedUnit() * 5.0f);
          kick(m, p.index, loftedPass(p.pos, box), {});
          m.stats[t].passes++;
          m.pendingPasser = p.index;
        } else if (m.phase == Phase::Kickoff || m.phase == Phase::ThrowIn || m.phase == Phase::GoalKick) {
          float sc = 0.0f;
          const int to = choosePassTarget(m, p.index, {}, false, &sc);
          if (to >= 0) pass(m, p.index, to, false, m.phase == Phase::GoalKick && distXZ(p.pos, m.players[size_t(to)].pos) > 25.0f);
        } else {
          dir = carrierDecision(m, p.index);
        }
        if (m.owner != p.index) { m.phase = Phase::Play; movePlayer(p, flat(p.vel) * 0.8f); continue; }
        p.facing = length(dir) > 0.1f ? p.facing : p.facing;
        m.aiThinkTimer[t + 0] = 6;
        if (length(dir) > 0.1f) it.target = p.pos + dir * 5.0f;
        else it.target = p.pos + p.facing * 5.0f;
      } else {
        it.target = p.pos + normalizeOr(flat(goalCenter(m, t) - p.pos), p.facing) * 5.0f;
      }
      const bool dash = pressureOn(m, p.index) > 0.3f || toAttack(m, t, p.pos).x > 20.0f;
      carrierStep(m, p, flat(it.target - p.pos), dash ? effectiveTop(p) * 0.92f : 5.0f, dash);
      continue;
    }
    // sem a bola
    const bool weHaveIt = ownerTeam == t && m.owner >= 0;
    if (p.role == Role::Goalkeeper) {
      it.target = shapeTarget(m, p, weHaveIt);
      const float reaction = 0.12f + 0.3f * (1.0f - p.attr.keeping);
      if (m.shotTeam == other(t) && m.owner < 0 && float(m.tick - m.shotTick) * kTick > reaction && float(m.tick - m.shotTick) * kTick < 2.0f) {
        const Vec3 line = ownGoal(m, t);
        it.target = {line.x - m.attackDir(t) * -0.6f, 0.0f, clampf(m.shotCross.z, -kGoalHalfWidth - 0.5f, kGoalHalfWidth + 0.5f)};
        it.target.x = line.x + m.attackDir(t) * 0.6f;
        it.sprint = true;
      }
      // bola solta perto do gol: o goleiro sai
      // bola solta perto do gol: o goleiro sai só se chega 0,65 s antes do atacante mais rápido (eFootball)
      if (m.owner < 0 && distXZ(m.ball.pos, ownGoal(m, t)) < 17.0f) {
        float rival = 1e9f;
        for (const Player& q : m.players) if (q.team != t) rival = std::min(rival, timeTo(q, m.ball.pos));
        if (timeTo(p, m.ball.pos) + 0.65f < rival) it.target = interceptPoint(m, p), it.sprint = true;
      }
    } else if (m.phase == Phase::Kickoff && (!live || p.team != m.restartTeam)) {
      it.target = p.pos;  // saída de bola: o adversário espera a bola rolar (sem bote no batedor)
    } else if (m.owner < 0 && p.index == m.passTarget) {
      it.target = receivePoint(m, p);
      it.sprint = true;
      // marcado (adversário a menos de 3 m): vem ao encontro da bola (eFootball v6.0), sem esperar parado
      float near = 99.0f;
      for (const Player& o : m.players) if (o.team != t) near = std::min(near, distXZ(o.pos, p.pos));
      if (near < 3.0f) it.target = p.pos + normalizeOr(flat(m.ball.pos - p.pos), p.facing) * 2.0f;
    } else if (!weHaveIt && p.index == chaser[t] && !(m.owner < 0 && m.passTarget >= 0 && teamOf(m.passTarget) == t)) {
      it.target = interceptPoint(m, p);
      it.sprint = true;
      // desarme: colado no portador que está com a bola colada
      if (m.owner >= 0 && distXZ(p.pos, m.ball.pos) < 1.3f && p.cooldown == 0 && m.rng.uniform() < 0.08f) {
        const Player& o = m.players[size_t(m.owner)];
        const float win = clampf(0.35f + 0.6f * (p.attr.defending - o.attr.dribbling), 0.1f, 0.85f);
        p.cooldown = 15;
        m.actionTick[size_t(p.index)] = m.tick;
        m.actionType[size_t(p.index)] = Match::ActTackle;
        if (m.rng.uniform() < win) {
          m.stats[t].tackles++;
          m.owner = -1;
          m.lastTouch = p.index;
          m.ball.vel = normalizeOr(flat(m.ball.pos - p.pos), p.facing) * 3.0f + Vec3{m.rng.signedUnit() * 2.0f, 0.0f, m.rng.signedUnit() * 2.0f};
          m.pendingPasser = -1;
        }
      }
    } else if (!weHaveIt && p.index == cover[t] && m.owner >= 0 && m.callPress[t]) {
      it.target = interceptPoint(m, p);  // 2º homem chamado para pressionar o portador
      it.sprint = true;
    } else if (!weHaveIt && p.index == cover[t] && m.owner >= 0) {
      // cobertura: entre a bola e o próprio gol
      it.target = m.ball.pos + normalizeOr(flat(ownGoal(m, t) - m.ball.pos), Vec3{}) * 6.0f;
      it.sprint = distXZ(p.pos, it.target) > 6.0f;
    } else {
      it.target = shapeTarget(m, p, weHaveIt);
      it.sprint = distXZ(p.pos, it.target) > 10.0f;
    }
    Vec3 v = arriveVel(p, it.target, it.sprint);
    if (p.role == Role::Goalkeeper && m.shotTeam == other(t) && m.owner < 0) v = v * 1.25f;
    turnTo(p, length(v) > 1.0f ? v : flat(m.ball.pos - p.pos), 0.25f);
    movePlayer(p, v);
  }

  // ---- jogador humano ----
  for (int t = 0; t < 2; ++t) {
    const int ci = m.controlled[t];
    if (ci < 0) continue;
    Player& p = m.players[size_t(ci)];
    const Input in = inputs ? inputs[t] : Input{};
    const Vec3 stick{float(in.dirX) / 127.0f, 0.0f, float(in.dirZ) / 127.0f};
    const bool sprint = in.buttons & Input::Sprint;
    m.humanStick[t] = stick;
    m.humanDash[t] = sprint;
    // Barra de força: o primeiro dos três botões segurado carrega; dispara ao soltar ou com a barra cheia (1 s).
    const uint8_t kickButtons = Input::Pass | Input::Through | Input::Shoot;
    const uint8_t held = in.buttons & kickButtons;
    uint8_t fire = 0;
    if (!held) {
      if (m.chargeButton[t] && !m.chargeFired[t] && (m.prevButtons[t] & m.chargeButton[t])) fire = m.chargeButton[t];
      m.chargeFired[t] = false;
    } else if (!m.chargeFired[t]) {
      if (!m.chargeButton[t] || !(held & m.chargeButton[t])) { m.chargeButton[t] = held & Input::Shoot ? Input::Shoot : held & Input::Through ? Input::Through : Input::Pass; m.charge[t] = 0; }
      m.charge[t] = std::min(m.charge[t] + 1, kTicksPerSecond);
      if (m.charge[t] >= kTicksPerSecond) { fire = m.chargeButton[t]; m.chargeFired[t] = true; }
    }
    const float power = m.chargeLevel(t);
    const bool lob = in.buttons & Input::Lob || m.prevButtons[t] & Input::Lob;
    const bool special = in.buttons & Input::Special || m.prevButtons[t] & Input::Special;
    const bool canAct = live && !((m.phase == Phase::Kickoff || restart) && m.phaseTicks < 10);
    const bool attacking = (m.owner >= 0 && teamOf(m.owner) == t) || (m.owner < 0 && m.passTarget >= 0 && teamOf(m.passTarget) == t);
    if (fire && p.index == m.owner && canAct) {
      if (fire & Input::Shoot) {
        humanShoot(m, ci, stick, power, special);
      } else {
        const bool through = fire & Input::Through;
        const int to = choosePassTarget(m, ci, length(stick) > 0.2f ? stick : p.facing, through);
        if (to >= 0) pass(m, ci, to, through, lob, power);
      }
      if (m.owner != ci) m.phase = Phase::Play;
    } else if (fire && attacking && p.index != m.owner && m.owner < 0) {
      // de primeira: guarda o comando, executado no domínio (resolvePossession)
      m.queued[t] = fire;
      m.queuedPower[t] = power;
      m.queuedDir[t] = stick;
      m.queuedLob[t] = lob;
      m.queuedSpecial[t] = special;
    }
    if (!held) { m.charge[t] = 0; m.chargeButton[t] = 0; }
    if (!attacking) m.queued[t] = 0;  // de primeira só vale com a bola indo para o nosso time

    // movimento: andando ~60% da máxima; com bola um pouco menos; Acelerar = máxima
    const bool withBall = p.index == m.owner;
    Vec3 want = stick * (effectiveTop(p) * (sprint ? (withBall ? 0.92f : 1.0f) : (withBall ? 0.55f : 0.6f)));
    const bool opponentHasBall = m.owner >= 0 && teamOf(m.owner) != t;
    // recebedor: vai sozinho ao ponto onde alcança a bola, a não ser que o joystick mande outra coisa
    if (attacking && p.index != m.owner && m.owner < 0 && p.index == m.passTarget && length(stick) < 0.3f)
      want = arriveVel(p, receivePoint(m, p), true);
    // Pressão segurada (botão Passe na defesa) = Match-up: entre o portador e o gol, de frente para ele, a ~1,8 m,
    // espelhando; longe, corre até lá. O desarme sai sozinho quando a bola se afasta do pé dele (resolvePossession).
    m.callPress[t] = false;
    if (p.index != m.owner && (in.buttons & Input::Pass) && !attacking) {
      if (opponentHasBall) {
        const Player& o = m.players[size_t(m.owner)];
        const Vec3 guard = o.pos + normalizeOr(flat(ownGoal(m, t) - o.pos), Vec3{}) * 1.8f + flat(o.vel) * 0.25f;
        want = arriveVel(p, guard, true);
        if (distXZ(p.pos, guard) < 6.0f) want += flat(o.vel) * 0.5f;  // perto: acompanha o passo do atacante
        if (special) m.callPress[t] = true;  // deslizar a Pressão: o 2º homem vai pressionar também
      } else {
        want = arriveVel(p, interceptPoint(m, p), sprint);
      }
    }
    // Desarme (botão Chute na defesa): bote no portador ao alcance
    if (p.index != m.owner && (fire & Input::Shoot) && opponentHasBall && distXZ(p.pos, m.ball.pos) < 1.9f) {
      const Player& o = m.players[size_t(m.owner)];
      const float win = clampf(0.4f + 0.6f * (p.attr.defending - o.attr.dribbling), 0.1f, 0.9f);
      p.cooldown = 12;
      m.actionTick[size_t(p.index)] = m.tick;
      m.actionType[size_t(p.index)] = Match::ActTackle;
      if (m.rng.uniform() < win) {
        m.stats[t].tackles++;
        m.owner = -1; m.lastTouch = ci;
        m.ball.vel = p.facing * 3.5f;
        m.pendingPasser = -1;
      }
    }
    const Vec3 faceTo = opponentHasBall && (in.buttons & Input::Pass) ? flat(m.players[size_t(m.owner)].pos - p.pos)
                        : length(want) > 0.5f ? want : flat(m.ball.pos - p.pos);
    if (p.index == m.owner && m.phase == Phase::Play) {
      // condução por toques: o joystick escolhe a direção do próximo toque; o jogador vai atrás da bola
      const float spd = length(stick) < 0.2f ? 0.0f : sprint ? effectiveTop(p) * 0.92f : 5.0f * std::min(1.0f, length(stick) * 1.3f);
      carrierStep(m, p, length(stick) > 0.2f ? stick : p.facing, spd, sprint);
    } else if (p.index == m.owner) {
      turnTo(p, length(stick) > 0.2f ? stick : p.facing, 1.0f);  // bola parada (reposição): só mira
      movePlayer(p, {});
    } else {
      turnTo(p, faceTo, 0.35f);
      movePlayer(p, want);
    }
    m.prevButtons[t] = in.buttons;
  }

  // ---- separação (ninguém atravessa ninguém) ----
  for (size_t i = 0; i < m.players.size(); ++i)
    for (size_t j = i + 1; j < m.players.size(); ++j) {
      Player& a = m.players[i];
      Player& b = m.players[j];
      const Vec3 d = flat(b.pos - a.pos);
      const float l2 = dot(d, d);
      if (l2 < 0.7f * 0.7f && l2 > 1e-8f) {
        const float l = std::sqrt(l2);
        const Vec3 push = d / l * ((0.7f - l) * 0.5f);
        a.pos -= push;
        b.pos += push;
      }
    }

  // ---- bola ----
  if (m.owner < 0 || m.phase == Phase::Play) {
    m.prevBallPos = m.ball.pos;
    if (!(restart && m.owner >= 0) && !(m.phase == Phase::Kickoff)) stepBall(m.ball, m.ballParams, kTick);
  }
  if (m.phase == Phase::Kickoff && m.owner >= 0) {  // bola parada no centro até a saída; quem bate fica nela
    m.ball.pos = {0.0f, m.ballParams.radius, 0.0f};
    Player& kp = m.players[size_t(m.owner)];
    kp.pos = m.ball.pos - toWorld(m, kp.team, 0.4f, 0.0f);
    kp.pos.y = 0.0f;
    kp.vel = {};
  } else if (m.phase == Phase::Kickoff && m.owner < 0) {
    // Saída sem dono (não deveria acontecer): a bola fica viva em vez de congelar no centro para sempre.
    m.phase = Phase::Play;
  }
  if (restart && m.owner >= 0) {
    const Player& tp = m.players[size_t(m.owner)];
    m.ball.pos = tp.pos + tp.facing * 0.5f;
    m.ball.pos.y = m.ballParams.radius;
  }
  if (m.phase == Phase::Play && m.owner < 0 && m.ball.pos.y > 0.7f && m.ball.pos.y < 2.6f) {
    for (Player& p : m.players) {
      if (p.cooldown > 0 || p.role == Role::Goalkeeper) continue;
      if (distXZ(p.pos, m.ball.pos) > 1.1f) continue;
      const float dGoal = distXZ(p.pos, goalCenter(m, p.team));
      if (dGoal < 16.0f) {
        p.facing = normalizeOr(flat(goalCenter(m, p.team) - p.pos), p.facing);
        shoot(m, p.index, m.rng.signedUnit() * 2.5f, 0.4f + m.rng.uniform() * 1.4f, 0.35f);
        m.actionType[size_t(p.index)] = Match::ActHeader;
      } else {  // corta de cabeça para longe
        p.cooldown = 8;
        m.lastTouch = p.index;
        m.ball.vel = normalizeOr(flat(goalCenter(m, p.team) - p.pos), Vec3{1, 0, 0}) * 9.0f + Vec3{0.0f, 4.0f, 0.0f};
        m.pendingPasser = -1;
      }
      break;
    }
  }
  if (m.phase == Phase::Play) {
    keeperSaves(m);
    resolvePossession(m);
    checkBallOut(m);
    if (m.owner >= 0) { m.stats[teamOf(m.owner)].possessionTicks++; m.possessionAge++; }
  }
}

uint64_t hashMatch(const Match& m) {
  uint64_t h = hashBall(m.ball);
  auto mix = [&](const void* data, size_t n) {
    const unsigned char* c = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
  };
  for (const Player& p : m.players) {
    const float f[7] = {p.pos.x, p.pos.z, p.vel.x, p.vel.z, p.facing.x, p.facing.z, p.energy};
    mix(f, sizeof(f));
  }
  mix(m.score, sizeof(m.score));
  mix(&m.tick, sizeof(m.tick));
  mix(&m.owner, sizeof(m.owner));
  return h;
}

}  // namespace fcsim
