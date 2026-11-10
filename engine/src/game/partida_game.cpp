// FCLegacy no Legacy Engine: a simulação fcsim desenhada com os jogadores realistas, juiz e bandeirinhas, torcida,
// câmera de transmissão, toque e HUD no estilo de transmissão.
#include "game/partida_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/paths.h"

namespace game {

using namespace scene;

namespace {

constexpr float kTick = fcsim::kTick;
glm::vec3 g(const fcsim::Vec3& v) { return {v.x, v.y, v.z}; }
glm::vec4 rgb(int hex, float a = 1.0f) { return {float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, a}; }
const glm::vec4 kInk = rgb(0xffffff), kNavy = rgb(0x0b1450, 0.92f), kYellow = rgb(0xffe600), kCyan = rgb(0x1ee6f0),
                kPanel = rgb(0x090d18, 0.78f), kGoal = rgb(0x3ddc84), kBtn = rgb(0x1d7a3a, 0.82f), kBtnRim = rgb(0x0e4d22, 0.9f);
float wrapAngle(float a) {
  while (a > glm::pi<float>()) a -= glm::two_pi<float>();
  while (a < -glm::pi<float>()) a += glm::two_pi<float>();
  return a;
}
// Clipe de locomoção e a velocidade em que foi gravado.
struct Loco { const char* name; float speed; };
const Loco kLoco[] = {{"k_idle", 0.0f}, {"l_walk", 1.4f}, {"l_jog", 3.2f}, {"l_run", 5.5f}, {"l_sprint", 8.0f}};
glm::vec4 opaque(glm::vec3 c) { return {std::pow(c.r, 1.0f / 2.2f), std::pow(c.g, 1.0f / 2.2f), std::pow(c.b, 1.0f / 2.2f), 1.0f}; }

}  // namespace

// ------------------------------------------------------------------------------------------------- início

void PartidaGame::loadTeams() {
  teams_[0] = {"Flamengo", "FLA", {0.58f, 0.01f, 0.03f}, {0.012f, 0.012f, 0.014f}, {0.05f, 0.5f, 0.12f}, {}};
  teams_[1] = {"Palmeiras", "PAL", {0.02f, 0.28f, 0.08f}, {0.85f, 0.85f, 0.82f}, {0.08f, 0.18f, 0.62f}, {}};
  for (int t = 0; t < 2; ++t)
    for (int i = 0; i < 11; ++i) teams_[t].players[size_t(i)] = "Jogador " + std::to_string(i + 1);
  std::ifstream f(core::gameDir() + "/elencos.json");
  if (!f) return;
  try {
    const nlohmann::json j = nlohmann::json::parse(f);
    const auto& times = j.at("times");
    for (size_t t = 0; t < 2 && t < times.size(); ++t) {
      const auto& tj = times[t];
      TeamInfo& ti = teams_[t];
      ti.name = tj.value("nome", ti.name);
      ti.abbr = tj.value("sigla", ti.abbr);
      auto col = [&](const char* k, glm::vec3& c) { if (tj.contains(k)) { const auto& a = tj.at(k); c = {a[0].get<float>(), a[1].get<float>(), a[2].get<float>()}; } };
      col("camisa", ti.shirt); col("calcao", ti.shorts); col("goleiro", ti.keeper);
      if (tj.contains("jogadores")) {
        const auto& js = tj.at("jogadores");
        for (size_t i = 0; i < 11 && i < js.size(); ++i) ti.players[i] = js[i].get<std::string>();
      }
    }
  } catch (const std::exception& e) {
    LOG_WARN("elencos.json inválido (%s)", e.what());
  }
}

void PartidaGame::init(Scene& s, renderer::Renderer& r, bool autoplay, bool rayTracing) {
  autoplay_ = autoplay;
  if (!rayTracing && !partidaWorld().day) {  // à noite sem ray tracing: sombra de um refletor pelos shadow maps
    s.sunDirection = glm::normalize(glm::vec3(-30.0f, 40.0f, -25.0f));
    s.sunIlluminance = 1800.0f;
  }
  LOG_INFO("Partida: carregando fontes do HUD");
  if (ui_.load()) r.setUiAtlas(ui_.atlas(), ui_.atlasSize(), ui_.atlasSize());
  LOG_INFO("Partida: carregando os times");
  loadTeams();
  screen_ = Screen::Menu;
  screenTime_ = 0.0f;
  LOG_INFO("Partida: preparando o pontapé inicial");
  startMatch(s);
  LOG_INFO("Partida: pronta");
}

void PartidaGame::startMatch(Scene& s) {
  fcsim::MatchConfig cfg;
  cfg.seed = uint64_t(1 + screenTime_ * 1000.0f);
  // FCLEGACY_HUMANO=1 com --autoplay: começa sozinho mas com o time da casa no controle humano (testar o HUD sem tela)
  autoplay_ = autoplay_ && !std::getenv("FCLEGACY_HUMANO");
  cfg.humanTeam = (autoplay_ || autoTeam_) ? -1 : 0;
  cfg.teamAttr[0].passing = 0.72f; cfg.teamAttr[0].shooting = 0.68f; cfg.teamAttr[0].dribbling = 0.7f;
  cfg.teamAttr[1].passing = 0.7f; cfg.teamAttr[1].defending = 0.68f;
  fcsim::initMatch(match_, cfg);
  acc_ = 0.0f;
  paused_ = false;
  lastScore_[0] = lastScore_[1] = 0;
  PartidaWorld& W = partidaWorld();
  for (int i = 0; i < 22; ++i) {
    const fcsim::Player& p = match_.players[size_t(i)];
    const TeamInfo& t = teams_[p.team];
    const bool gk = p.role == fcsim::Role::Goalkeeper;
    setKit(s, W.players[size_t(i)], gk ? t.keeper : t.shirt, gk ? glm::vec3(0.02f) : t.shorts);
    Anim& a = anim_[size_t(i)];
    a = Anim{};
    a.pos = a.prevPos = g(p.pos);
    a.yaw = std::atan2(p.facing.x, p.facing.z);
    a.phase = float(i) * 0.137f;
  }
  const glm::vec3 offStart[3] = {{-6.0f, 0.0f, 8.0f}, {12.0f, 0.0f, -35.2f}, {-12.0f, 0.0f, 35.2f}};
  for (int k = 0; k < 3; ++k) { officials_[size_t(k)] = Anim{}; officials_[size_t(k)].pos = officials_[size_t(k)].prevPos = offStart[k]; }
  ballPos_ = ballPrev_ = g(match_.ball.pos);
  camInit_ = false;
  if (cheering_) crowdCheer(s, false);
}

// ------------------------------------------------------------------------------------------------- entrada

void PartidaGame::layoutButtons(glm::vec2 screen) {
  const float u = screen.y / 100.0f, W = screen.x, H = screen.y;
  using I = fcsim::Input;
  // mesmo arranjo do eFootball: Lançamento e Chute em cima, Passe e Acelerar embaixo
  buttons_ = {
      {"Chute", "Desarme", I::Shoot, I::Shoot, {W - 22 * u, H - 45.5f * u}, 9.6f * u},
      {"Lançam.", "Trocar", I::Through, I::Switch, {W - 44 * u, H - 40 * u}, 9.6f * u},
      {"Passe", "Pressão", I::Pass, I::Pass, {W - 49.7f * u, H - 17.7f * u}, 9.6f * u},
      {"Acelerar", "Acelerar", I::Sprint, I::Sprint, {W - 23.6f * u, H - 19.5f * u}, 10.5f * u},
  };
  topBtnR_ = 4.6f * u;
  pauseBtn_ = {W - 9 * u, 6 * u};
  autoBtn_ = {W - 21 * u, 6 * u};
}

void PartidaGame::readTouches(const std::vector<Touch>& touches, glm::vec2 screen) {
  layoutButtons(screen);
  input_ = fcsim::Input{};
  tapped_ = false;
  std::vector<glm::vec2> newTouches;
  for (const Touch& t : touches)
    if (std::find(prevTouchIds_.begin(), prevTouchIds_.end(), t.id) == prevTouchIds_.end()) { tapped_ = true; newTouches.push_back(t.pos); }
  prevTouchIds_.clear();
  for (const Touch& t : touches) prevTouchIds_.push_back(t.id);
  // botões do canto (pausa, jogo automático): só no toque novo
  for (const glm::vec2& p : newTouches) {
    if (screen_ != Screen::Playing) continue;
    if (glm::length(p - pauseBtn_) < topBtnR_ * 1.4f) { paused_ = !paused_; tapped_ = false; }
    else if (glm::length(p - autoBtn_) < topBtnR_ * 1.4f) {
      autoTeam_ = !autoTeam_;
      match_.cfg.humanTeam = (autoplay_ || autoTeam_) ? -1 : 0;
      tapped_ = false;
    }
  }
  const int owner = match_.owner;
  const bool weHaveBall = owner >= 0 && match_.players[size_t(owner)].team == 0;
  // joystick fixo no canto inferior esquerdo: qualquer dedo na metade esquerda o comanda
  const float u = screen.y / 100.0f, R = 13.0f * u;
  stickBase_ = {33.0f * u, screen.y - 23.0f * u};
  bool stickAlive = false;
  for (const Touch& t : touches)
    if (t.id == stickTouch_) { stickAlive = true; stickVec_ = (t.pos - stickBase_) / R; }
  if (!stickAlive) {
    stickTouch_ = -1;
    stickVec_ = glm::vec2(0.0f);
    for (const Touch& t : touches)
      if (t.pos.x < screen.x * 0.42f && t.pos.y > screen.y * 0.25f) { stickTouch_ = t.id; stickVec_ = (t.pos - stickBase_) / R; break; }
  }
  if (glm::length(stickVec_) > 1.0f) stickVec_ = glm::normalize(stickVec_);
  const glm::vec2 sv = glm::length(stickVec_) < 0.15f ? glm::vec2(0.0f) : stickVec_;
  // tela: direita = +x do campo, baixo = +z (a câmera fica do lado +z olhando para -z)
  input_.dirX = int8_t(glm::clamp(sv.x, -1.0f, 1.0f) * 127.0f);
  input_.dirZ = int8_t(glm::clamp(sv.y, -1.0f, 1.0f) * 127.0f);
  for (const Touch& t : touches) {
    if (t.id == stickTouch_) continue;
    for (const Button& b : buttons_)
      if (glm::length(t.pos - b.c) < b.r * 1.2f) input_.buttons |= weHaveBall ? b.bit : b.defenseBit;
  }
}

// ------------------------------------------------------------------------------------------------- desenho

void PartidaGame::animateBody(Scene& s, Player& P, Anim& a, glm::vec3 pos, glm::vec3 vel, float targetYaw, bool gk, float dt) {
  PartidaWorld& W = partidaWorld();
  Model& m = s.models[size_t(P.model)];
  const float speed = glm::length(glm::vec2(vel.x, vel.z));
  a.yaw = wrapAngle(a.yaw + wrapAngle(targetYaw - a.yaw) * std::min(1.0f, dt * 12.0f));
  if (!a.action.empty()) {
    a.actionT += dt;
    a.actionW = std::min(1.0f, a.actionW + dt / 0.08f);
    if (a.actionT > a.actionEnd) { a.actionW -= dt / 0.08f + dt / 0.2f; if (a.actionW <= 0.0f) { a.action.clear(); a.actionW = 0.0f; } }
  }
  m.layers.clear();
  const bool backwards = speed > 0.6f && (vel.x * std::sin(a.yaw) + vel.z * std::cos(a.yaw)) < -0.4f * speed;
  const float locoW = 1.0f - a.actionW;
  if (a.celebrate) {
    const ClipMeta& c = W.clips["k_comemora"];
    a.phase = std::fmod(a.phase + dt / std::max(c.dur, 0.1f), 1.0f);
    m.layers.push_back({m.findAnimation("k_comemora"), a.phase * c.dur, 1.0f});
  } else if (backwards) {
    const ClipMeta& c = W.clips["l_back"];
    a.phase = std::fmod(a.phase + dt * (speed / std::max(c.speed, 0.1f)) / std::max(c.dur, 0.1f), 1.0f);
    m.layers.push_back({m.findAnimation("l_back"), a.phase * c.dur, locoW});
  } else {
    int k = 0;
    while (k < 3 && speed > kLoco[k + 1].speed) ++k;
    const Loco& lo = kLoco[k];
    const Loco& hi = kLoco[k + 1];
    const float w = glm::clamp((speed - lo.speed) / (hi.speed - lo.speed), 0.0f, 1.0f);
    const char* idleName = gk ? "gk_idle" : "k_idle";
    const ClipMeta& cl = W.clips[k == 0 ? idleName : lo.name];
    const ClipMeta& ch = W.clips[hi.name];
    const float rateHi = ch.dur > 0.0f ? (speed / std::max(ch.speed, 0.1f)) / ch.dur : 0.0f;
    const float rateLo = k == 0 ? 1.0f / std::max(cl.dur, 0.1f) : (speed / std::max(lo.speed, 0.1f)) / std::max(cl.dur, 0.1f);
    a.phase = std::fmod(a.phase + dt * glm::mix(rateLo, rateHi, w), 1.0f);
    if (1.0f - w > 0.01f) m.layers.push_back({m.findAnimation(k == 0 ? idleName : lo.name), a.phase * cl.dur, locoW * (1.0f - w)});
    if (w > 0.01f) m.layers.push_back({m.findAnimation(hi.name), a.phase * ch.dur, locoW * w});
  }
  if (!a.action.empty() && a.actionW > 0.0f) {
    const ClipMeta& c = W.clips[a.action];
    m.layers.push_back({m.findAnimation(a.action), std::min(a.actionT, c.dur - 0.01f), a.actionW});
  }
  m.root = glm::rotate(glm::translate(glm::mat4(1.0f), {pos.x, 0.0f, pos.z}), a.yaw, glm::vec3(0, 1, 0)) * P.fix;
}

void PartidaGame::animatePlayers(Scene& s, float dt, float alpha) {
  PartidaWorld& W = partidaWorld();
  auto meta = [&](const char* n) -> const ClipMeta* { const auto it = W.clips.find(n); return it == W.clips.end() ? nullptr : &it->second; };
  for (int i = 0; i < 22; ++i) {
    const fcsim::Player& sp = match_.players[size_t(i)];
    Anim& a = anim_[size_t(i)];
    const glm::vec3 pos = glm::mix(a.prevPos, a.pos, alpha);
    // ação nova (passe, chute, cabeceio, defesa)
    if (match_.actionTick[size_t(i)] != a.lastAction && match_.actionTick[size_t(i)] > 0) {
      a.lastAction = match_.actionTick[size_t(i)];
      const uint8_t type = match_.actionType[size_t(i)];
      auto play = [&](const char* clip, float from, float until) { a.action = clip; a.actionT = std::max(0.0f, from); a.actionEnd = until; };
      if (type == fcsim::Match::ActSave) {
        const glm::vec3 right(std::cos(a.yaw), 0.0f, -std::sin(a.yaw));
        const float lateral = glm::dot(g(match_.ball.pos) - pos, right);
        const char* clip = std::abs(lateral) < 0.8f ? "gk_pega" : lateral > 0.0f ? "gk_mergulho_d" : "gk_mergulho_e";
        if (const ClipMeta* c = meta(clip)) play(clip, c->reachT - 0.12f, c->dur);
      } else if (type == fcsim::Match::ActShot) {
        if (const ClipMeta* c = meta("k_kick")) play("k_kick", c->contact - 0.14f, c->contact + 0.6f);
      } else if (type == fcsim::Match::ActHeader) {
        if (const ClipMeta* c = meta("a_header")) play("a_header", c->contact - 0.2f, c->contact + 0.5f);
      } else if (type == fcsim::Match::ActPass || type == fcsim::Match::ActLob) {
        if (const ClipMeta* c = meta("a_pass")) play("a_pass", c->contact - 0.12f, c->contact + 0.45f);
      }
    }
    animateBody(s, W.players[size_t(i)], a, pos, g(sp.vel), std::atan2(sp.facing.x, sp.facing.z), sp.role == fcsim::Role::Goalkeeper, dt);
  }
  for (int k = 0; k < 3; ++k) {
    Anim& a = officials_[size_t(k)];
    const glm::vec3 toBall = ballPos_ - a.pos;
    animateBody(s, W.officials[size_t(k)], a, a.pos, a.vel, std::atan2(toBall.x, toBall.z), false, dt);
  }
}

// Juiz na diagonal, a ~12 m do lance; bandeirinhas na lateral, cada um cuidando de uma metade do campo.
void PartidaGame::moveOfficials(float dt) {
  const glm::vec3 b = ballPos_;
  const glm::vec3 targets[3] = {
      {glm::clamp(b.x - 7.0f, -42.0f, 42.0f), 0.0f, glm::clamp(b.z * 0.4f + 9.0f, -26.0f, 26.0f)},
      {glm::clamp(b.x, 0.0f, 50.0f), 0.0f, -35.2f},
      {glm::clamp(b.x, -50.0f, 0.0f), 0.0f, 35.2f}};
  for (int k = 0; k < 3; ++k) {
    Anim& a = officials_[size_t(k)];
    glm::vec3 want = (targets[k] - a.pos) * 1.2f;
    const float l = glm::length(want);
    if (l > 7.0f) want *= 7.0f / l;
    a.vel += (want - a.vel) * std::min(1.0f, dt * 3.0f);
    a.pos += a.vel * dt;
  }
}

void PartidaGame::drawBall(Scene& s, float dt, float alpha) {
  PartidaWorld& W = partidaWorld();
  const glm::vec3 p = glm::mix(ballPrev_, ballPos_, alpha);
  const glm::vec3 w = g(match_.ball.spin);
  const float wl = glm::length(w);
  if (wl > 1e-3f) ballRot_ = glm::normalize(glm::angleAxis(wl * dt, w / wl) * ballRot_);
  s.setTransform(W.ball, glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(ballRot_) * glm::scale(glm::mat4(1.0f), glm::vec3(kBallRadius)));
  const int k = p.x < 0.0f ? 0 : 1;
  const glm::vec3 inNet = glm::vec3(glm::inverse(W.netTransform[k]) * glm::vec4(p, 1.0f));
  s.meshes[W.netMesh[k]].clothColliders[0] = glm::vec4(inNet, kBallRadius + 0.03f);
  s.meshes[W.netMesh[1 - k]].clothColliders[0] = glm::vec4(0.0f);
}

// Gol: a torcida inteira troca para o quadro de comemoração (braços para cima).
void PartidaGame::crowdCheer(Scene& s, bool cheer) {
  PartidaWorld& W = partidaWorld();
  cheering_ = cheer;
  for (size_t i = 0; i < W.crowdMaterials.size(); ++i) {
    Material& m = s.materials[W.crowdMaterials[i]];
    m.uvOffset.x = (float(i % 8) + (cheer ? 8.0f : 0.0f)) / 16.0f;
    s.markMaterialDirty(W.crowdMaterials[i]);
  }
}

void PartidaGame::cameraUpdate(float dt, Camera& cam, glm::vec2 screen) {
  // câmera de transmissão (como a "Ampla" do eFootball): alta na lateral, acompanhando a bola no comprimento
  const glm::vec3 b = ballPos_;
  const glm::vec3 want(glm::clamp(b.x, -34.0f, 34.0f), 0.0f, glm::clamp(b.z * 0.5f, -12.0f, 12.0f));
  if (!camInit_) { camTarget_ = want; camInit_ = true; }
  camTarget_ += (want - camTarget_) * std::min(1.0f, dt * 2.0f);
  cam.position = glm::vec3(camTarget_.x * 0.9f, 17.5f, 47.0f);
  cam.lookAt(camTarget_ + glm::vec3(0.0f, 0.0f, -5.0f));
  const float hfov = glm::radians(50.0f), aspect = screen.x / std::max(screen.y, 1.0f);
  cam.fovY = 2.0f * std::atan(std::tan(hfov * 0.5f) / std::max(aspect, 0.6f));
}

glm::vec2 PartidaGame::project(const Camera& cam, glm::vec3 p, glm::vec2 screen) const {
  const glm::vec4 c = cam.projection(screen.x / screen.y) * cam.view() * glm::vec4(p, 1.0f);
  const glm::vec2 ndc = glm::vec2(c) / c.w;
  return {(ndc.x * 0.5f + 0.5f) * screen.x, (ndc.y * 0.5f + 0.5f) * screen.y};
}

// ------------------------------------------------------------------------------------------------- HUD

void PartidaGame::drawGroundRing(const Camera& cam, glm::vec2 screen, glm::vec3 at, float radius, glm::vec4 color, float thickness) {
  const int n = 28;
  glm::vec2 prev = project(cam, at + glm::vec3(radius, 0.02f, 0.0f), screen);
  for (int i = 1; i <= n; ++i) {
    const float a = glm::two_pi<float>() * float(i) / float(n);
    const glm::vec2 p = project(cam, at + glm::vec3(radius * std::cos(a), 0.02f, radius * std::sin(a)), screen);
    ui_.line(prev, p, thickness, color);
    prev = p;
  }
}

void PartidaGame::drawMinimap(glm::vec2 screen) {
  const float u = screen.y / 100.0f;
  const glm::vec2 size(24.5f * u, 15.0f * u);
  const glm::vec2 o(screen.x * 0.5f - size.x * 0.5f, screen.y - size.y - 2.5f * u);
  ui_.rect(o, size, rgb(0x0f3a1c, 0.55f));
  ui_.frame(o, size, 0.25f * u, rgb(0xffffff, 0.75f));
  ui_.line({o.x + size.x * 0.5f, o.y}, {o.x + size.x * 0.5f, o.y + size.y}, 0.2f * u, rgb(0xffffff, 0.6f));
  ui_.ring(o + size * 0.5f, 2.2f * u, 0.2f * u, rgb(0xffffff, 0.6f), 24);
  for (int s = -1; s <= 1; s += 2) {
    const float bx = o.x + (s < 0 ? 0.0f : size.x - 3.9f * u);
    ui_.frame({bx, o.y + size.y * 0.5f - 4.4f * u}, {3.9f * u, 8.8f * u}, 0.2f * u, rgb(0xffffff, 0.6f));
  }
  auto map = [&](glm::vec3 w) { return o + glm::vec2((w.x / 105.0f + 0.5f) * size.x, (w.z / 68.0f + 0.5f) * size.y); };
  for (int i = 0; i < 22; ++i) {
    const int t = i / 11;
    const bool me = i == match_.controlled[0];
    ui_.disc(map(anim_[size_t(i)].pos), (me ? 0.85f : 0.6f) * u, me ? kCyan : (t == 0 ? opaque(teams_[0].shirt) : opaque(teams_[1].shirt)), 10);
  }
  ui_.disc(map(ballPos_), 0.55f * u, kYellow, 10);
  // barras de fôlego: o seu jogador (esquerda) e o adversário com a bola (direita)
  auto bar = [&](float x, float frac) {
    ui_.rect({x, o.y + 4.0f * u}, {4.4f * u, 11.0f * u}, rgb(0x1a1a1a, 0.55f));
    ui_.rect({x + 2.0f * u, o.y + 4.8f * u + 9.4f * u * (1.0f - frac)}, {0.5f * u, 9.4f * u * frac}, rgb(0xffffff, 0.95f));
  };
  const int ci = match_.controlled[0] >= 0 ? match_.controlled[0] : 9;
  const int opp = match_.owner >= 0 && match_.players[size_t(match_.owner)].team == 1 ? match_.owner : 20;
  bar(o.x - 6.0f * u, match_.players[size_t(ci)].energy);
  bar(o.x + size.x + 1.6f * u, match_.players[size_t(opp)].energy);
}

void PartidaGame::drawHud(const Camera& cam, glm::vec2 screen) {
  const float u = screen.y / 100.0f;
  if (fps_ > 0.0f) {  // desempenho (discreto, no topo)
    char f[16];
    std::snprintf(f, sizeof(f), "%.0f fps", fps_);
    ui_.text(f, {screen.x * 0.5f, 4.0f * u}, 3.2f * u, rgb(0xffffff, 0.7f), Font::Body, Align::Center);
  }
  // ---- placar no estilo de transmissão ----
  {
    const float x = 4 * u, y = 2.5f * u, h = 7.5f * u;
    const glm::vec4 c0 = opaque(teams_[0].shirt), c1 = opaque(teams_[1].shirt);
    ui_.rect({x, y}, {0.9f * u, h}, c0);
    ui_.rect({x + 0.9f * u, y}, {11 * u, h}, kNavy);
    ui_.text(teams_[0].abbr, {x + 6.4f * u, y + 5.6f * u}, 5.2f * u, kInk, Font::Display, Align::Center);
    char sc[8];
    std::snprintf(sc, sizeof(sc), "%d", match_.score[0]);
    ui_.rect({x + 11.9f * u, y}, {5.6f * u, h}, kYellow);
    ui_.text(sc, {x + 14.7f * u, y + 5.7f * u}, 5.6f * u, rgb(0x0b1450), Font::Display, Align::Center);
    std::snprintf(sc, sizeof(sc), "%d", match_.score[1]);
    ui_.rect({x + 17.9f * u, y}, {5.6f * u, h}, kYellow);
    ui_.text(sc, {x + 20.7f * u, y + 5.7f * u}, 5.6f * u, rgb(0x0b1450), Font::Display, Align::Center);
    ui_.rect({x + 23.5f * u, y}, {11 * u, h}, kNavy);
    ui_.text(teams_[1].abbr, {x + 29.0f * u, y + 5.6f * u}, 5.2f * u, kInk, Font::Display, Align::Center);
    ui_.rect({x + 34.5f * u, y}, {0.9f * u, h}, c1);
    // relógio de jogo mm:ss
    const float gm = match_.gameMinute();
    const int mm = int(gm), ss = int((gm - float(mm)) * 60.0f);
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%d:%02d", mm, ss);
    ui_.rect({x + 35.8f * u, y}, {12.5f * u, h}, kYellow);
    ui_.text(clock, {x + 42.05f * u, y + 5.6f * u}, 5.0f * u, rgb(0x0b1450), Font::Display, Align::Center);
    ui_.rect({x + 48.6f * u, y}, {7.5f * u, h}, kNavy);
    ui_.text("BR", {x + 52.35f * u, y + 5.5f * u}, 4.4f * u, kYellow, Font::Display, Align::Center);
  }
  // ---- nomes, triângulo e anel no pé ----
  const int ci = match_.controlled[0];
  if (ci >= 0 && screen_ == Screen::Playing) {
    const glm::vec3 feet = anim_[size_t(ci)].pos;
    drawGroundRing(cam, screen, feet, 0.75f, rgb(0xffffff, 0.9f), 0.35f * u);
    const glm::vec2 head = project(cam, feet + glm::vec3(0.0f, 2.05f, 0.0f), screen);
    ui_.triangle(head + glm::vec2(-1.3f * u, -2.4f * u), head + glm::vec2(1.3f * u, -2.4f * u), head, kCyan);
    ui_.textShadow(teams_[0].players[size_t(ci % 11)], {head.x, head.y - 3.4f * u}, 3.6f * u, kInk, Font::Body, Align::Center);
    const float charge = float(match_.charge[0]) / float(fcsim::kTicksPerSecond);
    if (charge > 0.0f) {
      ui_.rect({head.x - 5 * u, head.y - 9.5f * u}, {10 * u, 1.2f * u}, kPanel);
      ui_.rect({head.x - 5 * u, head.y - 9.5f * u}, {10 * u * glm::clamp(charge, 0.0f, 1.0f), 1.2f * u}, charge > 0.85f ? rgb(0xff4d5e) : kYellow);
    }
  }
  if (match_.owner >= 0 && match_.owner != ci && screen_ == Screen::Playing) {
    const fcsim::Player& op = match_.players[size_t(match_.owner)];
    const glm::vec2 head = project(cam, anim_[size_t(op.index)].pos + glm::vec3(0.0f, 2.05f, 0.0f), screen);
    ui_.textShadow(teams_[op.team].players[size_t(op.index % 11)], {head.x, head.y - 0.6f * u}, 3.4f * u, kInk, Font::Body, Align::Center);
  }
  if (screen_ == Screen::Playing) drawMinimap(screen);
  // ---- controles ----
  if (match_.cfg.humanTeam == 0 && screen_ == Screen::Playing) {
    ui_.disc(stickBase_, 13 * u, rgb(0x2a2a2a, 0.35f));
    ui_.ring(stickBase_, 13 * u, 0.5f * u, rgb(0xffffff, 0.25f));
    ui_.disc(stickBase_ + stickVec_ * 13.0f * u, 8 * u, rgb(0xe8e8e8, 0.92f));
    ui_.ring(stickBase_ + stickVec_ * 13.0f * u, 8 * u, 0.4f * u, rgb(0xb0b0b0, 0.9f));
    const bool attacking = match_.owner >= 0 && match_.players[size_t(match_.owner)].team == 0;
    // "Chute" vira "Afastar" quando a bola está no terço de defesa
    const bool ownThird = attacking && match_.attackDir(0) * match_.ball.pos.x < -17.5f;
    for (const Button& b : buttons_) {
      const uint8_t bit = attacking ? b.bit : b.defenseBit;
      const bool down = input_.buttons & bit;
      ui_.disc(b.c, b.r * 1.08f, kBtnRim);
      ui_.disc(b.c, b.r, down ? rgb(0x34b35a, 0.95f) : kBtn);
      ui_.ring(b.c, b.r * 0.8f, 0.3f * u, rgb(0xffffff, 0.12f));
      const char* label = attacking ? (b.bit == fcsim::Input::Shoot && ownThird ? "Afastar" : b.label) : b.defenseLabel;
      ui_.textShadow(label, {b.c.x, b.c.y + 1.4f * u}, 4.0f * u, kInk, Font::Body, Align::Center);
    }
  }
  if (screen_ == Screen::Playing) {  // jogo automático e pausa
    for (int k = 0; k < 2; ++k) {
      const glm::vec2 c = k == 0 ? autoBtn_ : pauseBtn_;
      const bool on = k == 0 ? autoTeam_ : paused_;
      ui_.rect(c - glm::vec2(topBtnR_), glm::vec2(topBtnR_ * 2.0f), rgb(on ? 0xffe600 : 0xffffff, on ? 0.85f : 0.55f));
      ui_.text(k == 0 ? "AUTO" : "II", {c.x, c.y + 1.3f * u}, (k == 0 ? 2.8f : 4.0f) * u, rgb(0x202020), Font::Display, Align::Center);
    }
  }
  // ---- reposição do seu time: o jogo espera o passe ----
  if (match_.cfg.humanTeam == 0 && screen_ == Screen::Playing && match_.owner >= 0 && match_.owner == match_.controlled[0] &&
      match_.phase != fcsim::Phase::Play) {
    const char* what = match_.phase == fcsim::Phase::Kickoff ? "Saída de bola" : match_.phase == fcsim::Phase::Corner ? "Escanteio"
                     : match_.phase == fcsim::Phase::ThrowIn ? "Lateral" : "Tiro de meta";
    ui_.rect({screen.x * 0.5f - 30 * u, 14 * u}, {60 * u, 8 * u}, kPanel);
    ui_.text(std::string(what) + ": toque Passe (ou Chute)", {screen.x * 0.5f, 19.6f * u}, 4.0f * u, kInk, Font::Body, Align::Center);
  }
  // ---- faixas ----
  const char* banner = nullptr;
  glm::vec4 bc = kYellow;
  if (match_.phase == fcsim::Phase::Goal) { banner = "GOOOL!"; bc = kGoal; }
  else if (match_.phase == fcsim::Phase::HalfTime) banner = "INTERVALO";
  else if (screen_ == Screen::End) banner = "FIM DE JOGO";
  else if (paused_) banner = "PAUSA";
  if (banner) {
    ui_.rect({0, screen.y * 0.38f}, {screen.x, 16 * u}, kPanel);
    ui_.textShadow(banner, {screen.x * 0.5f, screen.y * 0.38f + 12 * u}, 12 * u, bc, Font::Display, Align::Center);
  }
  if (screen_ == Screen::Menu) {
    ui_.rect({0, 0}, screen, rgb(0x090d18, 0.55f));
    ui_.textShadow("FCLEGACY", {screen.x * 0.5f, screen.y * 0.36f}, 16 * u, kInk, Font::Display, Align::Center);
    ui_.textShadow(teams_[0].name + "  x  " + teams_[1].name, {screen.x * 0.5f, screen.y * 0.5f}, 6 * u, kYellow, Font::Display, Align::Center);
    ui_.textShadow("Toque para jogar  ·  2 tempos de 3 minutos", {screen.x * 0.5f, screen.y * 0.62f}, 4 * u, rgb(0xc8cede), Font::Body, Align::Center);
  }
  if (screen_ == Screen::End)
    ui_.textShadow("Toque para jogar de novo", {screen.x * 0.5f, screen.y * 0.62f}, 4.2f * u, rgb(0xc8cede), Font::Body, Align::Center);
}

// ------------------------------------------------------------------------------------------------- quadro

float PartidaGame::update(float realDt, const std::vector<Touch>& touches, Scene& s, Camera& cam, renderer::Renderer& r, glm::vec2 screen) {
  const float dt = std::min(realDt, 0.1f);
  screenTime_ += dt;
  if (realDt > 0.0f) fps_ = fps_ <= 0.0f ? 1.0f / realDt : fps_ + (1.0f / realDt - fps_) * 0.05f;
  const bool wasPaused = paused_;
  readTouches(touches, screen);
  if (screen_ == Screen::Menu && (tapped_ || ((autoplay_ || std::getenv("FCLEGACY_HUMANO")) && screenTime_ > 0.3f))) { screen_ = Screen::Playing; startMatch(s); }
  if (screen_ == Screen::End && (tapped_ && screenTime_ > 1.0f)) { screen_ = Screen::Playing; startMatch(s); }
  if (wasPaused && paused_ && tapped_) paused_ = false;  // pausado: qualquer toque volta ao jogo

  const bool running = screen_ == Screen::Playing && !paused_;
  if (running) {
    acc_ += dt;
    int steps = 0;
    while (acc_ >= kTick && steps < 4) {
      for (int i = 0; i < 22; ++i) anim_[size_t(i)].prevPos = anim_[size_t(i)].pos;
      ballPrev_ = ballPos_;
      const fcsim::Input inputs[2] = {input_, fcsim::Input{}};
      fcsim::stepMatch(match_, inputs);
      for (int i = 0; i < 22; ++i) anim_[size_t(i)].pos = g(match_.players[size_t(i)].pos);
      ballPos_ = g(match_.ball.pos);
      for (int t = 0; t < 2; ++t)
        if (match_.score[t] != lastScore_[t]) {  // gol: o autor comemora e a torcida pula
          lastScore_[t] = match_.score[t];
          if (match_.lastTouch >= 0) anim_[size_t(match_.lastTouch)].celebrate = true;
          crowdCheer(s, true);
        }
      if (match_.phase == fcsim::Phase::Kickoff) {
        for (Anim& a : anim_) a.celebrate = false;
        if (cheering_) crowdCheer(s, false);
      }
      acc_ -= kTick;
      ++steps;
    }
    if (steps == 4) acc_ = 0.0f;
    if (fcsim::finished(match_)) { screen_ = Screen::End; screenTime_ = 0.0f; }
  }
  const float alpha = glm::clamp(acc_ / kTick, 0.0f, 1.0f);
  const float animDt = running ? dt : 0.0f;
  moveOfficials(animDt);
  animatePlayers(s, animDt, alpha);
  drawBall(s, animDt, alpha);
  cameraUpdate(dt, cam, screen);
  ui_.begin(&r.ui, screen);
  drawHud(cam, screen);
  if (autoplay_ && screen_ == Screen::End && screenTime_ > 3.0f) { screen_ = Screen::Playing; startMatch(s); }
  return running ? dt : 0.0f;
}

}  // namespace game
