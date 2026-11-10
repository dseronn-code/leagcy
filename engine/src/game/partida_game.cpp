// FCLegacy no Legacy Engine: a simulação fcsim desenhada com os jogadores realistas, câmera de TV, toque e HUD.
#include "game/partida_game.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "core/log.h"

namespace game {

using namespace scene;

namespace {

constexpr float kTick = fcsim::kTick;
glm::vec3 g(const fcsim::Vec3& v) { return {v.x, v.y, v.z}; }
glm::vec4 rgb(int hex, float a = 1.0f) { return {float((hex >> 16) & 255) / 255.0f, float((hex >> 8) & 255) / 255.0f, float(hex & 255) / 255.0f, a}; }
const glm::vec4 kAmber = rgb(0xffb020), kInk = rgb(0xf5f2ea), kMuted = rgb(0x9ba4ba), kPanel = rgb(0x090d18, 0.82f), kGoal = rgb(0x3ddc84);
float wrapAngle(float a) {
  while (a > glm::pi<float>()) a -= glm::two_pi<float>();
  while (a < -glm::pi<float>()) a += glm::two_pi<float>();
  return a;
}
// Clipe de locomoção e a velocidade em que foi gravado.
struct Loco { const char* name; float speed; };
const Loco kLoco[] = {{"k_idle", 0.0f}, {"l_walk", 1.4f}, {"l_jog", 3.2f}, {"l_run", 5.5f}, {"l_sprint", 8.0f}};

}  // namespace

// ------------------------------------------------------------------------------------------------- início

void PartidaGame::init(Scene& s, renderer::Renderer& r, bool autoplay, bool rayTracing) {
  autoplay_ = autoplay;
  if (!rayTracing) {  // sombra dos refletores pelos shadow maps (como no pênalti)
    s.sunDirection = glm::normalize(glm::vec3(-30.0f, 40.0f, -25.0f));
    s.sunIlluminance = 1800.0f;
  }
  if (ui_.load()) r.setUiAtlas(ui_.atlas(), ui_.atlasSize(), ui_.atlasSize());
  teams_[0] = {"Flamengo", "FLA", {0.58f, 0.01f, 0.03f}, {0.012f, 0.012f, 0.014f}, {0.05f, 0.5f, 0.12f}};
  teams_[1] = {"Palmeiras", "PAL", {0.02f, 0.28f, 0.08f}, {0.85f, 0.85f, 0.82f}, {0.08f, 0.18f, 0.62f}};
  screen_ = Screen::Menu;
  screenTime_ = 0.0f;
  startMatch(s);
}

void PartidaGame::startMatch(Scene& s) {
  fcsim::MatchConfig cfg;
  cfg.seed = uint64_t(1 + screenTime_ * 1000.0f);
  cfg.humanTeam = autoplay_ ? -1 : 0;
  cfg.teamAttr[0].passing = 0.72f; cfg.teamAttr[0].shooting = 0.68f; cfg.teamAttr[0].dribbling = 0.7f;
  cfg.teamAttr[1].passing = 0.7f; cfg.teamAttr[1].defending = 0.68f;
  fcsim::initMatch(match_, cfg);
  acc_ = 0.0f;
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
  ballPos_ = ballPrev_ = g(match_.ball.pos);
  camInit_ = false;
}

// ------------------------------------------------------------------------------------------------- entrada

void PartidaGame::layoutButtons(glm::vec2 screen) {
  const float u = screen.y / 100.0f, W = screen.x, H = screen.y;
  using I = fcsim::Input;
  buttons_ = {
      {"CHUTE", "DESARME", I::Shoot, I::Shoot, {W - 14 * u, H - 17 * u}, 9.5f * u},
      {"PASSE", "PRESSÃO", I::Pass, I::Pass, {W - 33 * u, H - 11 * u}, 8.5f * u},
      {"PROFUND.", "TROCAR", I::Through, I::Switch, {W - 15 * u, H - 39 * u}, 7.5f * u},
      {"ALTO", "ALTO", I::Lob, I::Lob, {W - 32 * u, H - 31 * u}, 6.5f * u},
      {"CORRER", "CORRER", I::Sprint, I::Sprint, {W - 50 * u, H - 12 * u}, 7.0f * u},
  };
}

void PartidaGame::readTouches(const std::vector<Touch>& touches, glm::vec2 screen) {
  layoutButtons(screen);
  input_ = fcsim::Input{};
  tapped_ = false;
  // dedo novo = toque (menus)
  for (const Touch& t : touches)
    if (std::find(prevTouchIds_.begin(), prevTouchIds_.end(), t.id) == prevTouchIds_.end()) tapped_ = true;
  prevTouchIds_.clear();
  for (const Touch& t : touches) prevTouchIds_.push_back(t.id);

  const int owner = match_.owner;
  const bool weHaveBall = owner >= 0 && match_.players[size_t(owner)].team == 0;
  // joystick: o primeiro dedo na metade esquerda; a base nasce onde o dedo tocou
  bool stickAlive = false;
  const float R = screen.y * 0.13f;
  for (const Touch& t : touches) {
    if (t.id == stickTouch_) { stickAlive = true; stickVec_ = (t.pos - stickBase_) / R; }
  }
  if (!stickAlive) {
    stickTouch_ = -1;
    stickVec_ = glm::vec2(0.0f);
    for (const Touch& t : touches)
      if (t.pos.x < screen.x * 0.42f) { stickTouch_ = t.id; stickBase_ = t.pos; break; }
  }
  if (glm::length(stickVec_) > 1.0f) {  // a base acompanha o dedo que passa do raio
    const glm::vec2 dir = glm::normalize(stickVec_);
    stickBase_ += dir * (glm::length(stickVec_) - 1.0f) * R;
    stickVec_ = dir;
  }
  const float dead = 0.15f;
  const glm::vec2 sv = glm::length(stickVec_) < dead ? glm::vec2(0.0f) : stickVec_;
  // tela: direita = +x do campo, baixo = +z (a câmera fica do lado +z olhando para -z)
  input_.dirX = int8_t(glm::clamp(sv.x, -1.0f, 1.0f) * 127.0f);
  input_.dirZ = int8_t(glm::clamp(sv.y, -1.0f, 1.0f) * 127.0f);
  for (const Touch& t : touches) {
    if (t.id == stickTouch_) continue;
    for (const Button& b : buttons_)
      if (glm::length(t.pos - b.c) < b.r * 1.25f) input_.buttons |= weHaveBall ? b.bit : b.defenseBit;
  }
}

// ------------------------------------------------------------------------------------------------- desenho

void PartidaGame::animatePlayers(Scene& s, float dt, float alpha) {
  PartidaWorld& W = partidaWorld();
  for (int i = 0; i < 22; ++i) {
    const fcsim::Player& sp = match_.players[size_t(i)];
    Player& P = W.players[size_t(i)];
    Anim& a = anim_[size_t(i)];
    Model& m = s.models[size_t(P.model)];
    const glm::vec3 pos = glm::mix(a.prevPos, a.pos, alpha);
    const glm::vec3 vel = g(sp.vel);
    const float speed = glm::length(glm::vec2(vel.x, vel.z));
    // corpo vira suavemente para onde a simulação aponta
    const float targetYaw = std::atan2(sp.facing.x, sp.facing.z);
    a.yaw = wrapAngle(a.yaw + wrapAngle(targetYaw - a.yaw) * std::min(1.0f, dt * 12.0f));
    const bool gk = sp.role == fcsim::Role::Goalkeeper;

    // ação nova (passe, chute, cabeceio, defesa)
    if (match_.actionTick[size_t(i)] != a.lastAction && match_.actionTick[size_t(i)] > 0) {
      a.lastAction = match_.actionTick[size_t(i)];
      const uint8_t type = match_.actionType[size_t(i)];
      auto play = [&](const char* clip, float from, float until) {
        a.action = clip; a.actionT = std::max(0.0f, from); a.actionEnd = until;
      };
      auto meta = [&](const char* n) -> const ClipMeta* { const auto it = W.clips.find(n); return it == W.clips.end() ? nullptr : &it->second; };
      if (type == fcsim::Match::ActSave) {
        const glm::vec3 right(std::cos(a.yaw), 0.0f, -std::sin(a.yaw));
        const float lateral = glm::dot(g(match_.ball.pos) - pos, right);
        const bool center = std::abs(lateral) < 0.8f;
        const char* clip = center ? "gk_pega" : lateral > 0.0f ? "gk_mergulho_d" : "gk_mergulho_e";
        if (const ClipMeta* c = meta(clip)) play(clip, c->reachT - 0.12f, c->dur);
      } else if (type == fcsim::Match::ActShot) {
        if (const ClipMeta* c = meta("k_kick")) play("k_kick", c->contact - 0.14f, c->contact + 0.6f);
      } else if (type == fcsim::Match::ActHeader) {
        if (const ClipMeta* c = meta("a_header")) play("a_header", c->contact - 0.2f, c->contact + 0.5f);
      } else if (type == fcsim::Match::ActPass || type == fcsim::Match::ActLob) {
        if (const ClipMeta* c = meta("a_pass")) play("a_pass", c->contact - 0.12f, c->contact + 0.45f);
      }
    }
    if (!a.action.empty()) {
      a.actionT += dt;
      a.actionW = std::min(1.0f, a.actionW + dt / 0.08f);
      if (a.actionT > a.actionEnd) { a.actionW -= dt / 0.08f + dt / 0.2f; if (a.actionW <= 0.0f) { a.action.clear(); a.actionW = 0.0f; } }
    }

    // locomoção: mistura dos dois clipes vizinhos pela velocidade; pé no ritmo da velocidade real
    m.layers.clear();
    const bool backwards = speed > 0.6f && (vel.x * std::sin(a.yaw) + vel.z * std::cos(a.yaw)) < -0.4f * speed;
    const float locoW = 1.0f - a.actionW;
    if (a.celebrate) {
      m.layers.push_back({m.findAnimation("k_comemora"), std::fmod(a.phase * 3.0f, 3.1f), 1.0f});
      a.phase += dt / 3.0f;
    } else if (backwards) {
      const ClipMeta& c = W.clips["l_back"];
      a.phase = std::fmod(a.phase + dt * (speed / c.speed) / c.dur, 1.0f);
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
      // fase comum: cada clipe anda no ritmo da velocidade dele
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
}

void PartidaGame::drawBall(Scene& s, float dt, float alpha) {
  PartidaWorld& W = partidaWorld();
  const glm::vec3 p = glm::mix(ballPrev_, ballPos_, alpha);
  const glm::vec3 w = g(match_.ball.spin);
  const float wl = glm::length(w);
  if (wl > 1e-3f) ballRot_ = glm::normalize(glm::angleAxis(wl * dt, w / wl) * ballRot_);
  s.setTransform(W.ball, glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(ballRot_) * glm::scale(glm::mat4(1.0f), glm::vec3(kBallRadius)));
  // a bola empurra a rede do gol mais perto (colisor do tecido, no espaço do pano)
  const int k = p.x < 0.0f ? 0 : 1;
  const glm::vec3 inNet = glm::vec3(glm::inverse(W.netTransform[k]) * glm::vec4(p, 1.0f));
  s.meshes[W.netMesh[k]].clothColliders[0] = glm::vec4(inNet, kBallRadius + 0.03f);
  s.meshes[W.netMesh[1 - k]].clothColliders[0] = glm::vec4(0.0f);
}

void PartidaGame::cameraUpdate(float dt, Camera& cam, glm::vec2 screen) {
  // câmera de transmissão: alta, na lateral (+z), acompanhando a bola no comprimento
  const glm::vec3 b = ballPos_;
  const glm::vec3 want(glm::clamp(b.x, -36.0f, 36.0f), 0.0f, glm::clamp(b.z * 0.45f, -14.0f, 14.0f));
  if (!camInit_) { camTarget_ = want; camInit_ = true; }
  camTarget_ += (want - camTarget_) * std::min(1.0f, dt * 2.2f);
  // dentro da arquibancada principal, abaixo da cobertura (o telhado fica a ~15 m e cobre z de 41 a 58)
  cam.position = glm::vec3(camTarget_.x * 0.85f, 13.0f, 49.0f);
  cam.lookAt(camTarget_ + glm::vec3(0.0f, 0.0f, -2.0f));
  const float hfov = glm::radians(44.0f), aspect = screen.x / std::max(screen.y, 1.0f);
  cam.fovY = 2.0f * std::atan(std::tan(hfov * 0.5f) / std::max(aspect, 0.6f));
}

glm::vec2 PartidaGame::project(const Camera& cam, glm::vec3 p, glm::vec2 screen) const {
  const glm::vec4 c = cam.projection(screen.x / screen.y) * cam.view() * glm::vec4(p, 1.0f);
  const glm::vec2 ndc = glm::vec2(c) / c.w;
  return {(ndc.x * 0.5f + 0.5f) * screen.x, (ndc.y * 0.5f + 0.5f) * screen.y};
}

// ------------------------------------------------------------------------------------------------- HUD

void PartidaGame::drawHud(const Camera& cam, glm::vec2 screen) {
  const float u = screen.y / 100.0f;
  // placar
  {
    const float x = 3 * u, y = 3 * u, h = 8 * u;
    ui_.rect({x, y}, {44 * u, h}, kPanel);
    const int minute = std::min(90 + 9, int(match_.gameMinute()));
    char clock[16];
    std::snprintf(clock, sizeof(clock), "%d'", minute);
    char score[32];
    std::snprintf(score, sizeof(score), "%d - %d", match_.score[0], match_.score[1]);
    ui_.text(teams_[0].abbr, {x + 2 * u, y + 5.8f * u}, 5.2f * u, kInk, Font::Display);
    ui_.text(score, {x + 22 * u, y + 5.8f * u}, 5.6f * u, kAmber, Font::Display, Align::Center);
    ui_.text(teams_[1].abbr, {x + 32 * u, y + 5.8f * u}, 5.2f * u, kInk, Font::Display);
    ui_.rect({x + 44 * u, y}, {11 * u, h}, rgb(0xf5f2ea, 0.92f));
    ui_.text(clock, {x + 49.5f * u, y + 5.8f * u}, 4.8f * u, rgb(0x090d18), Font::Display, Align::Center);
  }
  // jogador controlado: marcador sobre a cabeça e a barra de força
  const int ci = match_.controlled[0];
  if (ci >= 0) {
    const glm::vec3 head = anim_[size_t(ci)].pos + glm::vec3(0.0f, 2.25f, 0.0f);
    const glm::vec2 p = project(cam, head, screen);
    ui_.disc(p, 1.1f * u, kAmber, 3);
    ui_.disc(p + glm::vec2(0.0f, -0.4f * u), 0.7f * u, kAmber, 20);
    const float charge = float(match_.charge[0]) / float(fcsim::kTicksPerSecond);
    if (charge > 0.0f) {
      ui_.rect({p.x - 5 * u, p.y - 4 * u}, {10 * u, 1.2f * u}, kPanel);
      ui_.rect({p.x - 5 * u, p.y - 4 * u}, {10 * u * glm::clamp(charge, 0.0f, 1.0f), 1.2f * u}, charge > 0.85f ? rgb(0xff4d5e) : kAmber);
    }
  }
  // controles
  if (match_.cfg.humanTeam == 0) {
    if (stickTouch_ >= 0) {
      ui_.disc(stickBase_, screen.y * 0.13f, rgb(0xffffff, 0.10f));
      ui_.ring(stickBase_, screen.y * 0.13f, 0.4f * u, rgb(0xffffff, 0.35f));
      ui_.disc(stickBase_ + stickVec_ * screen.y * 0.13f, 5 * u, rgb(0xffffff, 0.45f));
    } else {
      const glm::vec2 hint(20 * u, screen.y - 22 * u);
      ui_.ring(hint, screen.y * 0.13f, 0.4f * u, rgb(0xffffff, 0.18f));
    }
    const bool attacking = match_.owner >= 0 && match_.players[size_t(match_.owner)].team == 0;
    for (const Button& b : buttons_) {
      const uint8_t bit = attacking ? b.bit : b.defenseBit;
      const bool down = input_.buttons & bit;
      ui_.disc(b.c, b.r, rgb(down ? 0xffb020 : 0x090d18, down ? 0.75f : 0.45f));
      ui_.ring(b.c, b.r, 0.35f * u, rgb(0xffffff, 0.55f));
      ui_.text(attacking ? b.label : b.defenseLabel, {b.c.x, b.c.y + 1.2f * u}, 2.9f * u, kInk, Font::Display, Align::Center);
    }
  }
  // reposição do seu time: o jogo espera o passe
  if (match_.cfg.humanTeam == 0 && screen_ == Screen::Playing && match_.owner >= 0 && match_.owner == match_.controlled[0] &&
      match_.phase != fcsim::Phase::Play) {
    const char* what = match_.phase == fcsim::Phase::Kickoff ? "Saída de bola" : match_.phase == fcsim::Phase::Corner ? "Escanteio"
                     : match_.phase == fcsim::Phase::ThrowIn ? "Lateral" : "Tiro de meta";
    ui_.rect({screen.x * 0.5f - 30 * u, 14 * u}, {60 * u, 9 * u}, kPanel);
    ui_.text(std::string(what) + ": toque PASSE (ou CHUTE)", {screen.x * 0.5f, 20.2f * u}, 4.2f * u, kInk, Font::Body, Align::Center);
  }
  // faixas: gol, intervalo, fim
  const char* banner = nullptr;
  glm::vec4 bc = kAmber;
  if (match_.phase == fcsim::Phase::Goal) { banner = "GOOOL!"; bc = kGoal; }
  else if (match_.phase == fcsim::Phase::HalfTime) banner = "INTERVALO";
  else if (screen_ == Screen::End) banner = "FIM DE JOGO";
  if (banner) {
    ui_.rect({0, screen.y * 0.38f}, {screen.x, 16 * u}, kPanel);
    ui_.textShadow(banner, {screen.x * 0.5f, screen.y * 0.38f + 12 * u}, 12 * u, bc, Font::Display, Align::Center);
  }
  if (screen_ == Screen::Menu) {
    ui_.rect({0, 0}, screen, rgb(0x090d18, 0.55f));
    ui_.textShadow("FCLEGACY", {screen.x * 0.5f, screen.y * 0.36f}, 16 * u, kInk, Font::Display, Align::Center);
    ui_.textShadow(teams_[0].name + "  x  " + teams_[1].name, {screen.x * 0.5f, screen.y * 0.5f}, 6 * u, kAmber, Font::Display, Align::Center);
    ui_.textShadow("Toque para jogar  ·  2 tempos de 3 minutos", {screen.x * 0.5f, screen.y * 0.62f}, 4 * u, kMuted, Font::Body, Align::Center);
  }
  if (screen_ == Screen::End)
    ui_.textShadow("Toque para jogar de novo", {screen.x * 0.5f, screen.y * 0.62f}, 4.2f * u, kMuted, Font::Body, Align::Center);
}

// ------------------------------------------------------------------------------------------------- quadro

float PartidaGame::update(float realDt, const std::vector<Touch>& touches, Scene& s, Camera& cam, renderer::Renderer& r, glm::vec2 screen) {
  const float dt = std::min(realDt, 0.1f);
  screenTime_ += dt;
  readTouches(touches, screen);
  if (screen_ == Screen::Menu && (tapped_ || (autoplay_ && screenTime_ > 0.3f))) { screen_ = Screen::Playing; startMatch(s); }
  if (screen_ == Screen::End && (tapped_ && screenTime_ > 1.0f)) { screen_ = Screen::Playing; startMatch(s); }

  if (screen_ == Screen::Playing) {
    acc_ += dt;
    int steps = 0;
    while (acc_ >= kTick && steps < 4) {
      for (int i = 0; i < 22; ++i) anim_[size_t(i)].prevPos = anim_[size_t(i)].pos;
      ballPrev_ = ballPos_;
      const fcsim::Input inputs[2] = {input_, fcsim::Input{}};
      fcsim::stepMatch(match_, inputs);
      for (int i = 0; i < 22; ++i) anim_[size_t(i)].pos = g(match_.players[size_t(i)].pos);
      ballPos_ = g(match_.ball.pos);
      // gol: o autor comemora
      for (int t = 0; t < 2; ++t)
        if (match_.score[t] != lastScore_[t]) {
          lastScore_[t] = match_.score[t];
          if (match_.lastTouch >= 0) anim_[size_t(match_.lastTouch)].celebrate = true;
        }
      if (match_.phase == fcsim::Phase::Kickoff) for (Anim& a : anim_) a.celebrate = false;
      acc_ -= kTick;
      ++steps;
    }
    if (steps == 4) acc_ = 0.0f;
    if (fcsim::finished(match_)) { screen_ = Screen::End; screenTime_ = 0.0f; }
  }
  const float alpha = glm::clamp(acc_ / kTick, 0.0f, 1.0f);
  animatePlayers(s, dt, alpha);
  drawBall(s, dt, alpha);
  cameraUpdate(dt, cam, screen);
  ui_.begin(&r.ui, screen);
  drawHud(cam, screen);
  if (autoplay_ && screen_ == Screen::End && screenTime_ > 3.0f) { screen_ = Screen::Playing; startMatch(s); }
  return dt;
}

}  // namespace game
