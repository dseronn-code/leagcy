// Pênalti da Treta no Legacy Engine: regras, IA, lance animado (corrida, chute, mergulho, bola, rede), câmera de
// transmissão e HUD. Mesmas regras da versão web: canto + força para o batedor, canto + reflexo para o goleiro.
#include "game/penalti_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/log.h"

namespace game {

using namespace scene;

namespace {

std::mt19937& rng() {
  static std::mt19937 r(20261106);
  return r;
}
float rnd(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng()); }
template <class T> const T& pickOne(const std::vector<T>& v) { return v[size_t(rnd(0.0f, float(v.size()) - 0.001f))]; }
float easeOut(float t) { return 1.0f - std::pow(1.0f - t, 3.0f); }
float easeInOut(float t) { return t < 0.5f ? 2.0f * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 2.0f) * 0.5f; }
glm::vec4 rgb(int hex, float a = 1.0f) { return {float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, a}; }

const float kColX[3] = {-2.45f, 0.0f, 2.45f}, kRowY[2] = {1.78f, 0.52f};  // linha 0 = alto
const glm::vec3 kSpot(0.0f, kBallRadius, kSpotZ);
const glm::vec3 kKickerStart(-1.55f, 0.0f, 11.95f), kKickerBall(-0.42f, 0.0f, 11.38f), kKeeperHome(0.0f, 0.0f, 0.35f);
constexpr float kKickTime = 0.95f, kChooseSeconds = 7.0f;

const glm::vec4 kAmber = rgb(0xffb020), kInk = rgb(0xf5f2ea), kMuted = rgb(0x9ba4ba), kGoal = rgb(0x3ddc84), kMiss = rgb(0xff4d5e),
                kSave = rgb(0x5cc8ff), kPanel = rgb(0x090d18, 0.86f);

enum OutcomeType { kGol, kAngulo, kCavadinha, kDefesa, kCavadinhaPega, kFora, kTrave };
const char* kWord[] = {"GOOOL!", "NO ÂNGULO!", "CAVADINHA!", "DEFENDEU!", "PEGOU!", "ISOLOU!", "NA TRAVE!"};
const std::vector<std::string> kFalas[] = {
    {"{k} mandou o goleiro pro lado errado.", "Bola de um lado, goleiro do outro.", "{k} bateu com frieza."},
    {"Onde a coruja dorme!", "O goleiro nem viu a cor da bola.", "Pintura de {k}."},
    {"{k} humilhou em rede nacional.", "Cavadinha de craque."},
    {"Leu a mente de {k}!", "Muralha! Fechou o gol.", "{k}, previsível demais."},
    {"Pegou a cavadinha! Que papelão, {k}.", "Tentou humilhar e foi humilhado."},
    {"{k} isolou! A bola tá em órbita.", "Chutou pra arquibancada."},
    {"No ferro! {k} sentiu o poste.", "Trave! Quase."}};

glm::vec3 bez(glm::vec3 p0, glm::vec3 c, glm::vec3 p1, float s) {
  return (1 - s) * (1 - s) * p0 + 2 * (1 - s) * s * c + s * s * p1;
}
glm::vec3 bezD(glm::vec3 p0, glm::vec3 c, glm::vec3 p1, float s) { return 2 * (1 - s) * (c - p0) + 2 * s * (p1 - c); }

}  // namespace

// ------------------------------------------------------------------------------------------------- regras

PenaltiGame::Outcome PenaltiGame::resolve(const Choice& k, const std::string& power, const Choice& sv, const std::string& r) const {
  const bool sameZone = k.col == sv.col && k.row == sv.row, sameCol = k.col == sv.col;
  if (power == "isolou") return {k.row == 1 && k.col != 1 ? kTrave : kFora, false, false};
  if (k.col == 1 && k.row == 0 && power == "fraco") return sv.col == 1 ? Outcome{kCavadinhaPega, false, false} : Outcome{kCavadinha, true, false};
  bool saved = false;
  if (sameZone) saved = power == "fraco" ? true : power == "bom" ? r != "atrasado" : r == "perfeito";
  else if (sameCol) saved = power == "fraco" ? r != "atrasado" : power == "bom" ? r == "perfeito" : false;
  if (saved) return {kDefesa, false, false};
  const bool angulo = power == "perfeito" && k.row == 0 && k.col != 1;
  return {angulo ? kAngulo : kGol, true, sameZone || (sameCol && r != "atrasado")};
}

int PenaltiGame::decided() const {
  auto goals = [](const std::vector<int>& v) { int g = 0; for (int x : v) g += x; return g; };
  const int ga = goals(kicks_[0]), gb = goals(kicks_[1]), la = int(kicks_[0].size()), lb = int(kicks_[1].size());
  if (la <= n_ && lb <= n_ && (la < n_ || lb < n_)) {
    if (ga + (n_ - la) < gb) return 1;
    if (gb + (n_ - lb) < ga) return 0;
    return -1;
  }
  if (la == lb && ga != gb) return ga > gb ? 0 : 1;
  return -1;
}

int PenaltiGame::pressure(int team) const {
  PenaltiGame copy = *this;
  for (int g = 0; g < 2; ++g) {
    copy.kicks_[team] = kicks_[team];
    copy.kicks_[team].push_back(g);
    if (copy.decided() >= 0) return 2;
  }
  return int(kicks_[0].size()) >= n_ && int(kicks_[1].size()) >= n_ ? 1 : 0;
}

std::vector<PenaltiGame::Seg> PenaltiGame::segments(bool kick, int over) const {
  if (kick) {
    const float q = glm::clamp((float(over) - 60.0f) / 39.0f, 0.0f, 1.0f), iso = 0.13f - 0.07f * q, perf = 0.08f + 0.12f * q;
    return {{"fraco", 0.3f, rgb(0xe8c547)}, {"bom", 1.0f - iso - perf, rgb(0x5a8fd8)}, {"perfeito", 1.0f - iso, kGoal}, {"isolou", 1.0f, kMiss}};
  }
  return {{"atrasado", 0.2f, kMiss}, {"bom", 0.42f, rgb(0x5a8fd8)}, {"perfeito", 0.58f, kGoal}, {"bom", 0.8f, rgb(0x5a8fd8)}, {"atrasado", 1.0f, kMiss}};
}

// ------------------------------------------------------------------------------------------------- início

void PenaltiGame::init(Scene& s, renderer::Renderer& r, bool autoplay) {
  autoplay_ = autoplay;
  if (ui_.load()) r.setUiAtlas(ui_.atlas(), ui_.atlasSize(), ui_.atlasSize());
  team_[0] = {"Treta FC", "TFC", {1.0f, 0.42f, 0.01f}, {0.01f, 0.01f, 0.012f}, {0.02f, 0.22f, 0.65f},
              {{"Neymar", 99}, {"Gabigol", 95}, {"Memphis", 94}, {"Hulk", 93}, {"Kaio Jorge", 92}}};
  team_[1] = {"Flamengo", "FLA", {0.58f, 0.008f, 0.025f}, {0.01f, 0.01f, 0.012f}, {0.05f, 0.5f, 0.12f},
              {{"Jorginho", 96}, {"Pedro", 94}, {"Arrascaeta", 93}, {"Bruno Henrique", 88}, {"De la Cruz", 87}}};
  state_ = State::Menu;
  stateTime_ = 0.0f;
  resetPoses(s);
}

void PenaltiGame::resetPoses(Scene& s) {
  World& W = world();
  Model& kk = s.models[size_t(W.kicker.model)];
  kk.root = glm::rotate(glm::translate(glm::mat4(1.0f), kKickerStart), glm::pi<float>() - 0.45f, glm::vec3(0, 1, 0));
  kk.animation = W.kicker.idle;
  kk.speed = 1.0f;
  for (int i = 0; i < 2; ++i) W.kicker.armDir[i] = W.kicker.foreArmDir[i] = W.kicker.upLegDir[i] = W.kicker.legDir[i] = W.kicker.footDir[i] = glm::vec3(0.0f);
  W.kicker.spineDir = glm::vec3(0.0f);
  Model& gk = s.models[size_t(W.keeper.model)];
  gk.root = glm::translate(glm::mat4(1.0f), kKeeperHome);
  gk.animation = W.keeper.idle;
  shot_ = Shot{};
  shot_.ballPos = kSpot;
  s.setTransform(W.ball, glm::scale(glm::translate(glm::mat4(1.0f), kSpot), glm::vec3(kBallRadius)));
  s.meshes[W.netMesh].clothColliders[0] = glm::vec4(0.0f);
  poseKeeperReady(0.0f);
}

void PenaltiGame::poseKeeperReady(float t) {
  Player& k = world().keeper;
  const float b = std::sin(t * 3.2f) * 0.08f;
  for (int i = 0; i < 2; ++i) {
    const float sx = i == 0 ? -1.0f : 1.0f;  // o goleiro olha para +z: a direita dele fica em -x
    k.armDir[i] = {0.78f * sx, -0.32f - b, 0.5f};
    k.foreArmDir[i] = {0.42f * sx, 0.08f, 0.9f};
    k.upLegDir[i] = {0.3f * sx, -1.0f, 0.32f};
    k.legDir[i] = {0.1f * sx, -1.0f, -0.24f};
    k.footDir[i] = glm::vec3(0.0f);
  }
  k.spineDir = {0.0f, 1.0f, 0.12f};
}

// ------------------------------------------------------------------------------------------------- turnos

void PenaltiGame::nextTurn(Scene& s) {
  resetPoses(s);
  const int total = int(kicks_[0].size() + kicks_[1].size());
  kickTeam_ = (first_ + total) % 2;
  const Team& kt = team_[kickTeam_];
  kicker_ = kt.kickers[size_t(kicks_[kickTeam_].size()) % kt.kickers.size()];
  setKit(s, world().kicker, kt.shirt, kt.shorts);
  setKit(s, world().keeper, team_[1 - kickTeam_].keeperShirt, {0.01f, 0.01f, 0.012f});
  zoneChosen_ = meterRunning_ = false;
  autoplayDelay_ = rnd(0.6f, 1.6f);
  state_ = State::Pass;
  stateTime_ = 0.0f;
  camAim_ = true;
}

void PenaltiGame::finishChoice(bool timeout) {
  const bool kick = state_ == State::ChooseKick;
  const auto segs = segments(kick, kick ? kicker_.over : 75);
  std::string grade = kick ? "fraco" : "atrasado";
  if (!zoneChosen_) { choice_.col = int(rnd(0, 2.999f)); choice_.row = int(rnd(0, 1.999f)); }
  else if (meterRunning_) {
    const float p = std::fmod((simTime_ - meterStart_) * meterSpeed_, 2.0f), v = p < 1.0f ? p : 2.0f - p;
    for (const Seg& sg : segs) if (v <= sg.end) { grade = sg.name; break; }
  }
  (void)timeout;
  if (kick) {
    kick_ = choice_; power_ = grade;
    // IA do goleiro: canto ao acaso, mas desconfia de quem repete o lado
    const size_t n = aiHistory_.size();
    save_.col = (n >= 2 && aiHistory_[n - 1] == aiHistory_[n - 2] && rnd(0, 1) < 0.55f) ? aiHistory_[n - 1] : (rnd(0, 1) < 0.42f ? 0 : rnd(0, 1) < 0.28f ? 1 : 2);
    save_.row = rnd(0, 1) < 0.58f ? 1 : 0;
    const float q = 0.95f, pp = 0.18f + 0.3f * q, pa = 0.32f - 0.22f * q, x = rnd(0, 1);
    reflex_ = x < pp ? "perfeito" : x < pp + pa ? "atrasado" : "bom";
    aiHistory_.push_back(kick_.col);
  } else {
    save_ = choice_; reflex_ = grade;
  }
  state_ = State::Shot;
  stateTime_ = 0.0f;
  shotPending_ = true;
}

// ------------------------------------------------------------------------------------------------- lance

void PenaltiGame::startShot(Scene& s) {
  (void)s;
  outcome_ = resolve(kick_, power_, save_, reflex_);
  Shot& sh = shot_;
  sh = Shot{};
  sh.ballPos = kSpot;
  const float sx = kColX[kick_.col] != 0.0f ? (kColX[kick_.col] > 0 ? 1.0f : -1.0f) : (rnd(0, 1) < 0.5f ? -1.0f : 1.0f);
  float curve = 0.0f, arc = 0.25f;
  const bool perf = power_ == "perfeito";
  if (outcome_.type == kCavadinha || outcome_.type == kCavadinhaPega) { sh.target = {rnd(-0.3f, 0.3f), 1.35f, 0.0f}; sh.F = 1.25f; arc = 3.2f; }
  else if (outcome_.type == kFora) { sh.target = {kColX[kick_.col] + rnd(-0.8f, 0.8f), rnd(3.1f, 3.9f), -0.5f}; sh.F = 0.55f; arc = 0.6f; curve = rnd(-0.6f, 0.6f); }
  else if (outcome_.type == kTrave) { sh.target = {sx * 3.6f, rnd(0.6f, 2.0f), 0.02f}; sh.F = 0.55f; curve = rnd(-0.5f, 0.5f); }
  else {
    const float x = kick_.col == 1 ? rnd(-0.6f, 0.6f) : sx * (perf ? rnd(3.0f, 3.25f) : rnd(1.6f, 2.9f));
    const float y = kick_.row == 0 ? (perf ? rnd(1.95f, 2.2f) : rnd(1.45f, 2.05f)) : rnd(0.25f, 0.75f);
    sh.target = {x, y, 0.0f};
    sh.F = power_ == "fraco" ? 0.95f : perf ? 0.46f : 0.6f;
    curve = power_ == "fraco" ? 0.0f : rnd(-0.9f, 0.9f);
  }
  sh.ctrl = 0.5f * (kSpot + sh.target) + glm::vec3(curve, arc, 0.0f);
  // goleiro: ponto das mãos no fim do mergulho
  const glm::vec3 zoneT(kColX[save_.col] * 1.05f, kRowY[save_.row], 0.3f);
  const bool catches = outcome_.type == kDefesa || outcome_.type == kCavadinhaPega;
  sh.contactS = catches ? 0.93f : -1.0f;
  sh.diveT = catches ? bez(kSpot, sh.ctrl, sh.target, 0.93f) : zoneT;
  if (!catches && outcome_.close) {
    sh.diveT = glm::mix(sh.diveT, sh.target, 0.6f);
    sh.diveT.x -= (sh.target.x >= 0 ? 1.0f : -1.0f) * 0.35f;
  }
  const float delay = reflex_ == "perfeito" ? 0.04f : reflex_ == "bom" ? 0.13f : sh.F * 0.72f;
  sh.diveStart = kKickTime + delay;
  if (catches) sh.diveStart = std::min(sh.diveStart, kKickTime + sh.F * sh.contactS - sh.D);
  if (outcome_.type == kCavadinha) sh.diveStart = kKickTime + 0.05f;
  camAim_ = false;
  camPosT_ = {2.0f, 1.25f, 15.4f};
  camLookT_ = {0.0f, 1.0f, 3.0f};
  camK_ = 2.2f;
}

void PenaltiGame::outcomeFx() {
  shot_.outcomeAt = shot_.t;
  shake_ = outcome_.goal ? 0.12f : outcome_.type == kTrave ? 0.16f : 0.08f;
  resultWord_ = kWord[outcome_.type];
  resultColor_ = outcome_.goal ? kGoal : (outcome_.type == kDefesa || outcome_.type == kCavadinhaPega) ? kSave : kMiss;
  std::string line = outcome_.goal && outcome_.close && outcome_.type == kGol ? std::string("Passou raspando a luva do goleiro!") : pickOne(kFalas[outcome_.type]);
  for (size_t p; (p = line.find("{k}")) != std::string::npos;) line.replace(p, 3, kicker_.name);
  resultLine_ = line;
}

void PenaltiGame::updateShot(float dt, Scene& s) {
  World& W = world();
  Shot& sh = shot_;
  sh.t += dt;
  const float t = sh.t;
  Player& K = W.kicker;
  Model& km = s.models[size_t(K.model)];
  // corrida e chute
  if (t < kKickTime) {
    const float p = t / kKickTime;
    const glm::vec3 pos = glm::mix(kKickerStart, kKickerBall, easeInOut(p));
    const glm::vec3 d = kKickerBall - kKickerStart;
    km.root = glm::rotate(glm::translate(glm::mat4(1.0f), pos), std::atan2(d.x, d.z), glm::vec3(0, 1, 0));
    km.animation = K.run;
    km.speed = 0.85f;
  } else {
    const float yaw = glm::pi<float>() - 0.25f;
    km.root = glm::rotate(glm::translate(glm::mat4(1.0f), kKickerBall), yaw, glm::vec3(0, 1, 0));
    km.animation = K.idle;
    const glm::vec3 f(std::sin(yaw), 0.0f, std::cos(yaw)), up(0, 1, 0), side = glm::normalize(glm::cross(up, f));
    const float k = glm::clamp((t - kKickTime + 0.12f) / 0.3f, 0.0f, 1.0f), e = easeOut(k);
    // perna direita (índice 0): armada atrás -> chute -> acompanha
    K.upLegDir[0] = glm::mix(-up * 0.85f - f * 0.55f, -up * 0.5f + f * 0.9f, e);
    K.legDir[0] = glm::mix(up * 0.15f - f, -up * 0.2f + f, e);
    K.footDir[0] = -up * 0.2f + f;
    K.upLegDir[1] = -up + f * 0.15f + side * 0.08f;
    K.legDir[1] = -up - f * 0.1f;
    K.armDir[1] = glm::normalize(side * 0.95f + up * 0.15f - f * 0.2f);   // braço esquerdo abre (equilíbrio)
    K.foreArmDir[1] = K.armDir[1];
    K.armDir[0] = glm::normalize(-side * 0.6f - up * 0.55f + f * 0.5f);
    K.foreArmDir[0] = K.armDir[0];
    K.spineDir = glm::normalize(up + f * 0.25f * (1.0f - e) - f * 0.15f * e);
    if (!sh.kicked) { sh.kicked = true; shake_ = 0.03f; }
    if (t > kKickTime + 1.3f) {  // volta a ficar de pé, olhando o lance
      for (int i = 0; i < 2; ++i) K.armDir[i] = K.foreArmDir[i] = K.upLegDir[i] = K.legDir[i] = K.footDir[i] = glm::vec3(0.0f);
      K.spineDir = glm::vec3(0.0f);
      if (outcome_.goal && kickTeam_ == 0) {  // comemora de braços para cima
        K.armDir[0] = {-0.35f, 1.0f, 0.1f}; K.armDir[1] = {0.35f, 1.0f, 0.1f};
        K.foreArmDir[0] = K.armDir[0]; K.foreArmDir[1] = K.armDir[1];
      }
    }
  }
  // bola
  if (sh.kicked && !sh.free) {
    const float sn = (t - kKickTime) / sh.F, stopAt = sh.contactS > 0 ? sh.contactS : 1.0f;
    if (sn >= 0.55f) tsTarget_ = 0.22f;
    if (sn >= stopAt) {
      sh.ballPos = bez(kSpot, sh.ctrl, sh.target, stopAt);
      sh.vel = bezD(kSpot, sh.ctrl, sh.target, stopAt) / sh.F;
      sh.free = true;
      if (outcome_.type == kCavadinhaPega) { sh.held = true; sh.vel = glm::vec3(0.0f); }
      else if (outcome_.type == kDefesa) sh.vel = {(sh.ballPos.x >= 0 ? 1.0f : -1.0f) * rnd(1.5f, 3.5f), rnd(2.5f, 5.0f), rnd(4.0f, 7.0f)};
      else if (outcome_.type == kTrave) sh.vel = {-(sh.target.x >= 0 ? 1.0f : -1.0f) * rnd(2.0f, 4.0f), rnd(0.5f, 2.5f), rnd(4.0f, 7.0f)};
      outcomeFx();
    } else {
      sh.ballPos = bez(kSpot, sh.ctrl, sh.target, std::max(sn, 0.0f));
      sh.ballRot = glm::angleAxis(dt * 38.0f, glm::vec3(-1, 0, 0)) * sh.ballRot;
    }
  } else if (sh.free) {
    if (sh.held) {
      const glm::mat4& r = s.models[size_t(W.keeper.model)].root;
      sh.ballPos = glm::vec3(r * glm::vec4(0.0f, 1.25f, 0.35f, 1.0f));
    } else {
      sh.vel.y -= 9.8f * dt;
      sh.ballPos += sh.vel * dt;
      const float sp = glm::length(sh.vel);
      if (sp > 0.01f) sh.ballRot = glm::angleAxis(sp * dt / kBallRadius, glm::normalize(glm::cross(glm::vec3(0, 1, 0), sh.vel / sp))) * sh.ballRot;
      if (sh.ballPos.y < kBallRadius) { sh.ballPos.y = kBallRadius; sh.vel.y = std::abs(sh.vel.y) * 0.45f; sh.vel.x *= 0.8f; sh.vel.z *= 0.8f; }
      if (outcome_.goal && sh.ballPos.z < 0.1f) {
        if (sh.ballPos.z < -kNetDepth + 0.15f) {
          sh.ballPos.z = -kNetDepth + 0.15f;
          sh.hitNet = true;
          sh.vel.z = std::abs(sh.vel.z) * 0.08f; sh.vel.x *= 0.4f; sh.vel.y *= 0.5f;
        }
        if (std::abs(sh.ballPos.x) > kGoalHalfWidth - 0.12f) { sh.ballPos.x = (sh.ballPos.x > 0 ? 1.0f : -1.0f) * (kGoalHalfWidth - 0.12f); sh.vel.x *= -0.1f; }
        if (sh.ballPos.y > kGoalHeight - 0.12f) { sh.ballPos.y = kGoalHeight - 0.12f; sh.vel.y = -std::abs(sh.vel.y) * 0.1f; }
      }
    }
  }
  s.setTransform(W.ball, glm::translate(glm::mat4(1.0f), sh.ballPos) * glm::mat4_cast(sh.ballRot) * glm::scale(glm::mat4(1.0f), glm::vec3(kBallRadius)));
  // a bola empurra a rede (colisor do tecido, no espaço do pano)
  const glm::vec3 inNet = glm::vec3(glm::inverse(W.netTransform) * glm::vec4(sh.ballPos, 1.0f));
  s.meshes[W.netMesh].clothColliders[0] = glm::vec4(inNet, kBallRadius + 0.03f);

  // goleiro
  Player& G = W.keeper;
  Model& gm = s.models[size_t(G.model)];
  const float dp = (t - sh.diveStart) / sh.D;
  if (dp < 0.0f) {
    gm.root = glm::translate(glm::mat4(1.0f), kKeeperHome + glm::vec3(std::sin(t * 1.6f) * 0.3f, 0.0f, 0.0f));
    poseKeeperReady(t);
  } else {
    const float e = easeOut(glm::clamp(dp, 0.0f, 1.0f));
    const glm::vec3 T = sh.diveT;
    gm.animation = G.idle;
    const bool low = T.y < 1.1f && std::abs(T.x) < 1.0f;
    if (low) {
      gm.root = glm::translate(glm::mat4(1.0f), kKeeperHome + glm::vec3(T.x * e, -0.28f * e, 0.0f));
      for (int i = 0; i < 2; ++i) {
        const float sx = i == 0 ? -1.0f : 1.0f;
        G.upLegDir[i] = {0.35f * sx, -0.7f, 0.65f};
        G.legDir[i] = {0.12f * sx, -1.0f, -0.35f};
        const glm::vec3 a = glm::normalize(T - (glm::vec3(gm.root[3]) + glm::vec3(0.2f * sx, 1.1f, 0.0f)));
        G.armDir[i] = a; G.foreArmDir[i] = a;
      }
      G.spineDir = {0.0f, 0.8f, 0.6f};
    } else {
      const float R = 2.1f, th = std::atan2(T.x, std::max(T.y, 0.1f));
      const glm::vec2 F(T.x - std::sin(th) * R, std::max(0.0f, T.y - std::cos(th) * R));
      float rot = -th * e, y = F.y * e + std::sin(glm::pi<float>() * glm::clamp(dp, 0.0f, 1.0f)) * 0.25f * std::min(1.0f, std::abs(th));
      if (dp > 1.0f) {  // cai no chão
        const float f = easeInOut(glm::clamp((dp - 1.0f) * sh.D / 0.45f, 0.0f, 1.0f)), side = th >= 0 ? 1.0f : -1.0f;
        if (std::abs(th) > 0.45f) { rot = glm::mix(-th, -side * 1.42f, f); y = glm::mix(F.y, 0.12f, f); }
        else y = glm::mix(F.y, 0.0f, f);
      }
      gm.root = glm::rotate(glm::translate(glm::mat4(1.0f), {glm::mix(kKeeperHome.x, F.x, e), y, kKeeperHome.z}), rot, glm::vec3(0, 0, 1));
      const glm::vec3 up(std::sin(-rot), std::cos(-rot), 0.0f), across(std::cos(-rot), -std::sin(-rot), 0.0f);
      for (int i = 0; i < 2; ++i) {
        const float sx = i == 0 ? -1.0f : 1.0f;
        const glm::vec3 a = glm::normalize(glm::mix(glm::vec3(0.78f * sx, -0.3f, 0.5f), up + across * 0.12f * sx + glm::vec3(0, 0, 0.12f), e));
        G.armDir[i] = a; G.foreArmDir[i] = a;
        G.upLegDir[i] = glm::normalize(-up + across * 0.22f * sx);
        G.legDir[i] = glm::normalize(-up + across * 0.12f * sx + glm::vec3(0, 0, -0.1f));
      }
      G.spineDir = up;
    }
  }

  // câmera de transmissão
  if (sh.kicked && !sh.free) {
    const glm::vec3 b = sh.ballPos;
    camPosT_ = {b.x * 0.5f + 1.1f, std::max(0.9f, b.y * 0.6f + 0.6f), std::max(4.2f, b.z + 3.6f)};
    camLookT_ = {b.x * 0.4f, 1.1f, 0.0f};
    camK_ = 3.5f;
  } else if (sh.free) {
    const float since = t - sh.outcomeAt;
    if (outcome_.goal) {
      if (since > 0.12f) { camPosT_ = {sh.ballPos.x * 0.4f + 1.8f + since * 0.3f, 1.25f, -3.6f}; camLookT_ = {sh.ballPos.x * 0.5f, 0.9f, 3.0f}; camK_ = 2.5f; }
    } else if (outcome_.type == kDefesa || outcome_.type == kCavadinhaPega) {
      const float kx = gm.root[3].x;
      camPosT_ = {kx * 0.6f + 2.4f, 1.2f, 5.2f}; camLookT_ = {kx * 0.8f, 0.9f, 0.3f}; camK_ = 2.2f;
    } else {
      camPosT_ = {2.5f, 1.6f, 7.5f};
      camLookT_ = {sh.ballPos.x * 0.5f, std::min(sh.ballPos.y, 4.0f), std::max(sh.ballPos.z, -6.0f)};
      camK_ = 2.2f;
    }
    if (since > 0.55f) tsTarget_ = 1.0f;
    if (since > 2.1f) sh.done = true;
  }
}

// ------------------------------------------------------------------------------------------------- câmera

namespace {
float fovFor(float halfWidthDeg, float aspect, float minV, float maxV) {
  const float v = 2.0f * std::atan(std::tan(glm::radians(halfWidthDeg)) / aspect);
  return glm::clamp(v, glm::radians(minV), glm::radians(maxV));
}
}  // namespace

void PenaltiGame::cameraAim(Camera& cam, glm::vec2 screen) {
  camPos_ = camPosT_ = {0.0f, 1.9f, 17.6f};
  camLook_ = camLookT_ = {0.0f, 1.0f, 0.0f};
  cam.position = camPos_;
  cam.lookAt(camLook_);
  cam.fovY = fovFor(17.5f, screen.x / screen.y, 34.0f, 80.0f);
}

void PenaltiGame::cameraUpdate(float dt, Camera& cam, glm::vec2 screen) {
  if (state_ == State::Menu || state_ == State::End) {  // volta lenta pelo estádio
    const float o = simTime_ * 0.07f;
    camPosT_ = {std::sin(o) * 15.0f, 5.5f + std::sin(o * 0.7f) * 1.5f, 9.0f + std::cos(o) * 11.0f};
    camLookT_ = {0.0f, 1.2f, 1.5f};
    camK_ = 1.2f;
  }
  if (camAim_ && (state_ == State::Pass || state_ == State::ChooseKick || state_ == State::ChooseSave)) { cameraAim(cam, screen); return; }
  const float a = 1.0f - std::exp(-dt * camK_);
  camPos_ = glm::mix(camPos_, camPosT_, a);
  camLook_ = glm::mix(camLook_, camLookT_, a);
  shake_ *= std::exp(-dt * 6.0f);
  cam.position = camPos_ + glm::vec3(rnd(-1, 1), rnd(-1, 1), rnd(-1, 1)) * shake_;
  cam.lookAt(camLook_);
  cam.fovY = fovFor(state_ == State::Shot ? 24.0f : 26.0f, screen.x / screen.y, 40.0f, 78.0f);
}

glm::vec2 PenaltiGame::project(const Camera& cam, glm::vec3 p, glm::vec2 screen) const {
  const glm::vec4 c = cam.projection(screen.x / screen.y) * cam.view() * glm::vec4(p, 1.0f);
  const glm::vec2 ndc = glm::vec2(c) / c.w;
  return {(ndc.x * 0.5f + 0.5f) * screen.x, (ndc.y * 0.5f + 0.5f) * screen.y};
}

// ------------------------------------------------------------------------------------------------- quadro

float PenaltiGame::update(float realDt, const Pointer& p, Scene& s, Camera& cam, renderer::Renderer& r, glm::vec2 screen) {
  timeScale_ = glm::mix(timeScale_, tsTarget_, 1.0f - std::exp(-realDt * 10.0f));
  const float dt = realDt * timeScale_;
  simTime_ += realDt;
  stateTime_ += realDt;
  handleInput(p, cam, screen);
  if (shotPending_) { shotPending_ = false; startShot(s); }

  switch (state_) {
    case State::Menu:
      if (startRequested_ || (autoplay_ && stateTime_ > 0.4f)) {
        startRequested_ = false;
        kicks_[0].clear(); kicks_[1].clear(); aiHistory_.clear();
        first_ = autoplay_ ? 0 : (rnd(0, 1) < 0.5f ? 0 : 1);
        nextTurn(s);
      }
      break;
    case State::Pass:
      if (stateTime_ > 1.1f) {
        state_ = kickTeam_ == 0 ? State::ChooseKick : State::ChooseSave;
        stateTime_ = 0.0f;
        const int pr = pressure(kickTeam_);
        meterSpeed_ = (state_ == State::ChooseKick ? 0.95f - 0.25f * glm::clamp((float(kicker_.over) - 60.0f) / 39.0f, 0.0f, 1.0f) : 0.95f) *
                      (pr == 2 ? 1.3f : pr == 1 ? 1.18f : 1.0f);
        if (state_ == State::ChooseSave) {  // a IA já escolheu o chute
          const float q = glm::clamp((float(kicker_.over) - 60.0f) / 39.0f, 0.0f, 1.0f), x = rnd(0, 1);
          const float pi = 0.1f - 0.07f * q, pp = 0.12f + 0.4f * q, pf = 0.18f - 0.12f * q;
          power_ = x < pi ? "isolou" : x < pi + pp ? "perfeito" : x < pi + pp + pf ? "fraco" : "bom";
          kick_.col = rnd(0, 1) < 0.4f ? 0 : rnd(0, 1) < 0.33f ? 1 : 2;
          kick_.row = rnd(0, 1) < 0.45f ? 0 : 1;
          if (rnd(0, 1) < 0.035f) { kick_ = {1, 0, 0}; power_ = "fraco"; }
        }
      }
      break;
    case State::ChooseKick:
    case State::ChooseSave:
      if (autoplay_) {
        if (!zoneChosen_ && stateTime_ > autoplayDelay_) {
          choice_.col = int(rnd(0, 2.999f)); choice_.row = int(rnd(0, 1.999f));
          zoneChosen_ = meterRunning_ = true;
          meterStart_ = simTime_;
        } else if (meterRunning_ && simTime_ - meterStart_ > autoplayDelay_ * 0.9f) finishChoice(false);
      }
      if ((state_ == State::ChooseKick || state_ == State::ChooseSave) && stateTime_ >= kChooseSeconds) finishChoice(true);
      if (shotPending_) { shotPending_ = false; startShot(s); }
      break;
    case State::Shot:
      updateShot(dt, s);
      if (shot_.done) { state_ = State::Result; stateTime_ = 0.0f; kicks_[kickTeam_].push_back(outcome_.goal ? 1 : 0); }
      break;
    case State::Result:
      updateShot(dt, s);
      if (stateTime_ > (autoplay_ ? 2.2f : 4.0f)) {
        tsTarget_ = 1.0f;
        if (decided() >= 0) { state_ = State::End; stateTime_ = 0.0f; resetPoses(s); }
        else nextTurn(s);
      }
      break;
    case State::End:
      if (autoplay_ && stateTime_ > 3.0f) { state_ = State::Menu; stateTime_ = 0.0f; }
      break;
  }
  if (state_ == State::Pass || state_ == State::ChooseKick || state_ == State::ChooseSave || state_ == State::Menu || state_ == State::End)
    poseKeeperReady(simTime_);
  cameraUpdate(realDt, cam, screen);
  ui_.begin(&r.ui, screen);
  drawHud(cam, screen);
  return dt;
}

void PenaltiGame::handleInput(const Pointer& p, Camera& cam, glm::vec2 screen) {
  if (!p.pressed || autoplay_) return;
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  switch (state_) {
    case State::Menu:
      startRequested_ = true;  // nextTurn precisa da cena: atendido no update
      break;
    case State::ChooseKick:
    case State::ChooseSave: {
      const glm::vec2 a = project(cam, {-kGoalHalfWidth, kGoalHeight, 0.0f}, screen), b = project(cam, {kGoalHalfWidth, 0.0f, 0.0f}, screen);
      if (p.pos.x >= a.x && p.pos.x <= b.x && p.pos.y >= a.y && p.pos.y <= b.y) {
        choice_.col = std::min(2, int((p.pos.x - a.x) / (b.x - a.x) * 3.0f));
        choice_.row = std::min(1, int((p.pos.y - a.y) / (b.y - a.y) * 2.0f));
        if (!zoneChosen_) { zoneChosen_ = meterRunning_ = true; meterStart_ = simTime_; }
        return;
      }
      if (zoneChosen_ && p.pos.y > screen.y - 16.0f * u) finishChoice(false);
      break;
    }
    case State::Result:
      if (stateTime_ > 0.9f) stateTime_ = 100.0f;
      break;
    case State::End:
      if (stateTime_ > 1.0f) { state_ = State::Menu; stateTime_ = 0.0f; }
      break;
    default:
      break;
  }
}

// ------------------------------------------------------------------------------------------------- HUD

void PenaltiGame::drawHud(Camera& cam, glm::vec2 screen) {
  switch (state_) {
    case State::Menu: drawMenu(screen); break;
    case State::Pass: {
      drawScoreboard(screen);
      const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
      const std::string title = kickTeam_ == 0 ? "SUA VEZ DE BATER" : "AGORA VOCÊ DEFENDE";
      const float a = glm::clamp(stateTime_ * 4.0f, 0.0f, 1.0f);
      ui_.rect({0.0f, screen.y * 0.42f - 9.0f * u}, {screen.x, 15.0f * u}, kPanel * glm::vec4(1, 1, 1, a));
      ui_.textShadow(title, {screen.x * 0.5f, screen.y * 0.42f}, 9.0f * u, kAmber * glm::vec4(1, 1, 1, a), Font::Display, Align::Center);
      ui_.text(kicker_.name + " (" + team_[kickTeam_].name + ")" + (pressure(kickTeam_) == 2 ? " · PODE ACABAR AQUI" : ""),
               {screen.x * 0.5f, screen.y * 0.42f + 4.5f * u}, 3.6f * u, kInk * glm::vec4(1, 1, 1, a), Font::Body, Align::Center);
      break;
    }
    case State::ChooseKick:
    case State::ChooseSave: drawScoreboard(screen); drawChoose(cam, screen); break;
    case State::Shot: drawScoreboard(screen); break;
    case State::Result: drawScoreboard(screen); drawResult(screen); break;
    case State::End: drawEnd(screen); break;
  }
}

void PenaltiGame::drawScoreboard(glm::vec2 screen) {
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  const float w = 94.0f * u, h = 12.0f * u, x = 0.5f * (screen.x - w), y = 4.0f * u;
  ui_.rect({x, y}, {w, h}, kPanel);
  const float sw = 16.0f * u;
  ui_.rect({x + 0.5f * (w - sw), y}, {sw, h}, kInk);
  auto goals = [](const std::vector<int>& v) { int g = 0; for (int k : v) g += k; return g; };
  char score[16];
  std::snprintf(score, sizeof(score), "%d-%d", goals(kicks_[0]), goals(kicks_[1]));
  ui_.text(score, {x + 0.5f * w, y + 0.78f * h}, 8.6f * u, rgb(0x060912), Font::Display, Align::Center);
  for (int t = 0; t < 2; ++t) {
    const bool right = t == 1;
    const float tx = right ? x + w - 2.5f * u : x + 2.5f * u;
    const glm::vec3 c = glm::pow(team_[t].shirt, glm::vec3(1.0f / 2.2f));
    const float sq = 2.2f * u;
    ui_.rect({right ? tx - sq : tx, y + 2.2f * u}, {sq, sq}, glm::vec4(c, 1.0f));
    ui_.text(team_[t].name, {right ? tx - sq - 1.2f * u : tx + sq + 1.2f * u, y + 4.6f * u}, 4.6f * u, kInk, Font::Display, right ? Align::Right : Align::Left);
    const int n = std::max<int>(n_, int(std::max(kicks_[0].size(), kicks_[1].size())));
    const float d = 2.6f * u, gap = 0.9f * u;
    for (int i = 0; i < n; ++i) {
      const float dx = right ? tx - (float(i) + 1.0f) * (d + gap) + gap : tx + float(i) * (d + gap);
      const glm::vec2 pos(dx, y + 7.0f * u);
      if (size_t(i) < kicks_[t].size()) ui_.rect(pos, {d, d}, kicks_[t][size_t(i)] ? kGoal : kMiss);
      else ui_.frame(pos, {d, d}, std::max(1.0f, 0.3f * u), kMuted);
    }
  }
}

void PenaltiGame::drawChoose(Camera& cam, glm::vec2 screen) {
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  const bool kick = state_ == State::ChooseKick;
  const glm::vec4 zc = kick ? kAmber : kSave;
  // zonas do gol
  const glm::vec2 a = project(cam, {-kGoalHalfWidth, kGoalHeight, 0.0f}, screen), b = project(cam, {kGoalHalfWidth, 0.0f, 0.0f}, screen);
  const glm::vec2 cell = (b - a) / glm::vec2(3.0f, 2.0f), gap(0.5f * u);
  for (int row = 0; row < 2; ++row)
    for (int col = 0; col < 3; ++col) {
      const glm::vec2 p = a + cell * glm::vec2(col, row) + gap, sz = cell - 2.0f * gap;
      const bool sel = zoneChosen_ && choice_.col == col && choice_.row == row;
      ui_.rect(p, sz, sel ? zc * glm::vec4(1, 1, 1, 0.35f) : glm::vec4(1, 1, 1, 0.07f));
      ui_.frame(p, sz, sel ? 0.6f * u : 0.3f * u, sel ? zc : glm::vec4(1, 1, 1, 0.55f));
    }
  // painel de baixo: título, tempo, medidor e botão
  const float remain = std::max(0.0f, kChooseSeconds - stateTime_);
  const bool low = remain <= 3.0f;
  const float px = 4.0f * u, pw = screen.x - 2.0f * px;
  float y = screen.y - (zoneChosen_ ? 44.0f : 22.0f) * u;
  ui_.rect({px, y}, {pw, 17.0f * u}, kPanel);
  const std::string title = kick ? kicker_.name + " na bola" : "Onde você vai pular?";
  ui_.text(title, {px + 3.0f * u, y + 7.2f * u}, 6.6f * u, kInk, Font::Display);
  char num[8];
  std::snprintf(num, sizeof(num), "%d", int(std::ceil(remain)));
  ui_.text(num, {px + pw - 3.0f * u, y + 7.2f * u}, 7.0f * u, low ? kMiss : kInk, Font::Display, Align::Right);
  ui_.rect({px + 3.0f * u, y + 9.4f * u}, {pw - 6.0f * u, 1.0f * u}, glm::vec4(1, 1, 1, 0.12f));
  ui_.rect({px + 3.0f * u, y + 9.4f * u}, {(pw - 6.0f * u) * remain / kChooseSeconds, 1.0f * u}, low ? kMiss : kAmber);
  const std::string sub = !zoneChosen_ ? (kick ? "Toque no canto do gol. Over " + std::to_string(kicker_.over) + "." : kicker_.name + " vai bater. Escolha o canto.")
                                       : (kick ? "Agora pare a força. Dá pra trocar o canto." : "Agora pare o reflexo no verde.");
  ui_.text(sub, {px + 3.0f * u, y + 14.6f * u}, 3.4f * u, kMuted);
  if (!zoneChosen_) return;
  y += 20.0f * u;
  const auto segs = segments(kick, kick ? kicker_.over : 75);
  const float mh = 6.0f * u;
  float prev = 0.0f;
  ui_.rect({px - 0.4f * u, y - 0.4f * u}, {pw + 0.8f * u, mh + 0.8f * u}, kInk);
  for (const Seg& sg : segs) {
    ui_.rect({px + prev * pw, y}, {(sg.end - prev) * pw, mh}, sg.color);
    if (sg.end - prev > 0.12f) ui_.textShadow(sg.name, {px + 0.5f * (prev + sg.end) * pw, y + mh + 3.6f * u}, 2.8f * u, kInk, Font::Body, Align::Center);
    prev = sg.end;
  }
  const float ph = std::fmod((simTime_ - meterStart_) * meterSpeed_, 2.0f), v = ph < 1.0f ? ph : 2.0f - ph;
  ui_.rect({px + v * pw - 0.6f * u, y - 1.0f * u}, {1.2f * u, mh + 2.0f * u}, glm::vec4(1.0f));
  y += mh + 6.5f * u;
  ui_.rect({px, y}, {pw, 11.0f * u}, kAmber);
  ui_.text(kick ? "CHUTAR!" : "PULAR!", {screen.x * 0.5f, y + 8.4f * u}, 8.0f * u, rgb(0x1b1203), Font::Display, Align::Center);
}

void PenaltiGame::drawMenu(glm::vec2 screen) {
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  ui_.rect({0.0f, screen.y * 0.3f}, {screen.x, 46.0f * u}, kPanel);
  ui_.textShadow("PÊNALTI", {screen.x * 0.5f, screen.y * 0.3f + 18.0f * u}, 20.0f * u, kInk, Font::Display, Align::Center);
  ui_.textShadow("DA TRETA", {screen.x * 0.5f, screen.y * 0.3f + 28.0f * u}, 11.0f * u, kAmber, Font::Display, Align::Center);
  ui_.text("Feito no Legacy Engine · ray tracing em tempo real", {screen.x * 0.5f, screen.y * 0.3f + 35.0f * u}, 3.3f * u, kMuted, Font::Body, Align::Center);
  const float pulse = 0.75f + 0.25f * std::sin(simTime_ * 4.0f);
  ui_.text("TOQUE PARA JOGAR", {screen.x * 0.5f, screen.y * 0.3f + 42.0f * u}, 5.0f * u, kAmber * glm::vec4(1, 1, 1, pulse), Font::Display, Align::Center);
}

void PenaltiGame::drawResult(glm::vec2 screen) {
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  const float k = glm::clamp(stateTime_ / 0.35f, 0.0f, 1.0f), sc = 1.0f + (1.0f - easeOut(k)) * 1.6f;
  ui_.textShadow(resultWord_, {screen.x * 0.5f, screen.y * 0.42f}, 18.0f * u * sc, resultColor_ * glm::vec4(1, 1, 1, k), Font::Display, Align::Center);
  if (stateTime_ > 0.35f) {
    const float w = std::min(screen.x - 8.0f * u, ui_.measure(resultLine_, 4.4f * u) + 8.0f * u);
    ui_.rect({0.5f * (screen.x - w), screen.y * 0.42f + 5.0f * u}, {w, 9.0f * u}, kPanel);
    ui_.text(resultLine_, {screen.x * 0.5f, screen.y * 0.42f + 11.0f * u}, 4.4f * u, kInk, Font::Body, Align::Center);
  }
}

void PenaltiGame::drawEnd(glm::vec2 screen) {
  const float u = std::min(screen.x, screen.y * 0.5625f) / 100.0f;
  const int w = decided();
  auto goals = [](const std::vector<int>& v) { int g = 0; for (int k : v) g += k; return g; };
  ui_.rect({0.0f, screen.y * 0.3f}, {screen.x, 42.0f * u}, kPanel);
  ui_.textShadow(w == 0 ? "VITÓRIA!" : "DERROTA", {screen.x * 0.5f, screen.y * 0.3f + 17.0f * u}, 17.0f * u, w == 0 ? kGoal : kMiss, Font::Display, Align::Center);
  char line[64];
  std::snprintf(line, sizeof(line), "%s %d × %d %s", team_[0].abbr.c_str(), goals(kicks_[0]), goals(kicks_[1]), team_[1].abbr.c_str());
  ui_.text(line, {screen.x * 0.5f, screen.y * 0.3f + 28.0f * u}, 8.0f * u, kInk, Font::Display, Align::Center);
  ui_.text("Toque para jogar de novo", {screen.x * 0.5f, screen.y * 0.3f + 37.0f * u}, 4.0f * u, kMuted, Font::Body, Align::Center);
}

}  // namespace game
