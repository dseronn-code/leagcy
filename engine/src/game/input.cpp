#include "game/input.h"

#include <cstdlib>
#include <sstream>

#include "core/log.h"

namespace game {

namespace {
const char* kAxes[6] = {"esquerdo_x", "esquerdo_y", "direito_x", "direito_y", "lt", "rt"};
}

int axisIndex(const std::string& name) {
  for (int i = 0; i < 6; ++i)
    if (name == kAxes[i]) return i;
  return -1;
}

float GameInput::axis(const std::string& name) const {
  const int i = axisIndex(name);
  return i < 0 ? 0.0f : axes[i];
}

std::vector<SimEvent> parseSimulated(const std::string& spec) {
  std::vector<SimEvent> out;
  std::stringstream all(spec);
  std::string item;
  while (std::getline(all, item, ';')) {
    if (item.empty()) continue;
    std::vector<std::string> parts;
    std::stringstream ss(item);
    std::string p;
    while (std::getline(ss, p, ':')) parts.push_back(p);
    if (parts.size() < 2) continue;
    SimEvent e;
    e.t = float(std::atof(parts[0].c_str()));
    e.kind = parts[1];
    auto xy = [](const std::string& s) {
      const size_t c = s.find(',');
      return c == std::string::npos ? glm::vec2(0.0f)
                                    : glm::vec2(float(std::atof(s.substr(0, c).c_str())), float(std::atof(s.substr(c + 1).c_str())));
    };
    if ((e.kind == "tecla" || e.kind == "segurar" || e.kind == "eixo") && parts.size() >= 3) e.name = parts[2];
    if (e.kind == "segurar" && parts.size() >= 4) e.value = float(std::atof(parts[3].c_str()));
    if (e.kind == "eixo" && parts.size() >= 4) e.value = float(std::atof(parts[3].c_str()));
    if ((e.kind == "clique" || e.kind == "mouse") && parts.size() >= 3) e.pos = xy(parts[2]);
    if (e.kind != "tecla" && e.kind != "segurar" && e.kind != "eixo" && e.kind != "clique" && e.kind != "mouse") {
      LOG_WARN("--simular: evento desconhecido '%s'", e.kind.c_str());
      continue;
    }
    out.push_back(e);
  }
  return out;
}

void updateInput(GameInput& in, const RawInput& raw, const std::vector<SimEvent>& sim, float tPrev, float tNow) {
  const std::unordered_set<std::string> prevHeld = in.held;
  const bool prevMouseDown = in.mouseDown;
  in.held = raw.held;
  in.mouseDown = raw.mouseDown && !in.simMouse;
  if (raw.mouse.x >= 0.0f && !in.simMouse) in.mouse = raw.mouse;
  in.pad = raw.pad;
  for (int i = 0; i < 6; ++i) in.axes[i] = raw.axes[i];
  bool simClick = false;
  std::vector<std::string> taps;
  for (const SimEvent& e : sim) {
    if (!(e.t > tPrev && e.t <= tNow)) continue;
    if (e.kind == "tecla") taps.push_back(e.name);
    else if (e.kind == "segurar") in.simHolds.emplace_back(e.name, e.t + e.value);
    else if (e.kind == "eixo") { if (int a = axisIndex(e.name); a >= 0) in.simAxes[a] = e.value; }
    else if (e.kind == "mouse") { in.mouse = e.pos; in.simMouse = true; }
    else if (e.kind == "clique") { in.mouse = e.pos; in.simMouse = true; simClick = true; }
  }
  for (auto it = in.simHolds.begin(); it != in.simHolds.end();) {
    if (tNow > it->second) { it = in.simHolds.erase(it); continue; }
    in.held.insert(it->first);
    ++it;
  }
  for (const auto& [a, v] : in.simAxes) in.axes[a] = v;
  if (!in.simAxes.empty()) in.pad = true;
  in.pressed.clear();
  for (const std::string& k : in.held)
    if (!prevHeld.count(k)) in.pressed.insert(k);
  for (const std::string& k : taps) {  // um quadro: apertada e solta
    in.pressed.insert(k);
    in.held.insert(k);
  }
  in.mousePressed = (in.mouseDown && !prevMouseDown) || simClick;
  if (simClick) in.mouseDown = true;
}

}  // namespace game
