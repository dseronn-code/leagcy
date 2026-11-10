#include "game/scripting.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>
#include <filesystem>
#include <map>
#include <sstream>
#include <vector>

#define SOL_LUA_VERSION 504
#include <sol/sol.hpp>

#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "game/world.h"
#include "scene/scene.h"

namespace game {

namespace {

// Entidade vista pelo Lua: o mundo e o identificador (cópia barata; inválida se a entidade sumir).
struct Entidade {
  World* world;
  entt::entity e;
  bool valid() const { return world && world->registry.valid(e); }
};

// Tabela Lua <-> JSON (salvar_jogo / carregar_jogo). Tabela com chaves 1..n vira lista; o resto, objeto.
nlohmann::json luaToJson(const sol::object& o, int depth = 0) {
  if (depth > 32) return nullptr;
  switch (o.get_type()) {
    case sol::type::boolean: return o.as<bool>();
    case sol::type::number: {
      const double d = o.as<double>();
      if (std::floor(d) == d && std::abs(d) < 9e15) return int64_t(d);
      return d;
    }
    case sol::type::string: return o.as<std::string>();
    case sol::type::table: {
      sol::table t = o.as<sol::table>();
      const size_t n = t.size();
      size_t count = 0;
      for (auto& kv : t) { (void)kv; ++count; }
      if (n > 0 && n == count) {
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 1; i <= n; ++i) arr.push_back(luaToJson(t[i], depth + 1));
        return arr;
      }
      nlohmann::json obj = nlohmann::json::object();
      for (auto& [k, v] : t) {
        std::string key = k.get_type() == sol::type::string ? k.as<std::string>() : luaToJson(k, depth + 1).dump();
        obj[key] = luaToJson(v, depth + 1);
      }
      return obj;
    }
    default: return nullptr;
  }
}

sol::object jsonToLua(const nlohmann::json& j, sol::this_state st) {
  sol::state_view L(st);
  switch (j.type()) {
    case nlohmann::json::value_t::boolean: return sol::make_object(L, j.get<bool>());
    case nlohmann::json::value_t::number_integer:
    case nlohmann::json::value_t::number_unsigned: return sol::make_object(L, j.get<int64_t>());
    case nlohmann::json::value_t::number_float: return sol::make_object(L, j.get<double>());
    case nlohmann::json::value_t::string: return sol::make_object(L, j.get<std::string>());
    case nlohmann::json::value_t::array: {
      sol::table t = L.create_table();
      for (size_t i = 0; i < j.size(); ++i) t[i + 1] = jsonToLua(j[i], st);
      return t;
    }
    case nlohmann::json::value_t::object: {
      sol::table t = L.create_table();
      for (auto& [k, v] : j.items()) t[k] = jsonToLua(v, st);
      return t;
    }
    default: return sol::make_object(L, sol::lua_nil);
  }
}

std::filesystem::file_time_type modified(const std::string& path) {
  std::error_code ec;
  auto t = std::filesystem::last_write_time(path, ec);
  return ec ? std::filesystem::file_time_type{} : t;
}

}  // namespace

struct Scripting::Impl {
  World& world;
  scene::Scene* scene = nullptr;  // válido durante update()
  sol::state lua;

  struct Instance {
    entt::entity e;
    std::string path;
    sol::environment env;
    bool started = false;
    bool failed = false;
  };
  std::vector<Instance> instances;
  Instance* current = nullptr;  // script rodando agora (incluir() carrega no ambiente dele)
  std::map<std::string, std::filesystem::file_time_type> stamps;
  float sinceCheck = 0.0f;

  explicit Impl(World& w) : world(w) {
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table, sol::lib::coroutine);
    bindApi();
  }

  void bindApi() {
    lua.new_usertype<Entidade>(
        "Entidade", sol::no_constructor,
        "nome", sol::property([](const Entidade& h) -> std::string {
          if (!h.valid()) return "";
          const Name* n = h.world->registry.try_get<Name>(h.e);
          return n ? n->value : "";
        }),
        "valida", &Entidade::valid,
        "posicao", [](const Entidade& h) {
          glm::vec3 p(0.0f);
          if (h.valid() && h.world->registry.all_of<Transform>(h.e)) p = h.world->registry.get<Transform>(h.e).position;
          return std::make_tuple(p.x, p.y, p.z);
        },
        "mover_para", [this](const Entidade& h, float x, float y, float z) {
          if (!h.valid() || !scene || !h.world->registry.all_of<Transform>(h.e)) return;
          h.world->setEntityPose(*scene, h.e, glm::vec3(x, y, z), h.world->registry.get<Transform>(h.e).rotation);
        },
        "girar", [this](const Entidade& h, float xDeg, float yDeg, float zDeg) {  // em graus, eixos do mundo
          if (!h.valid() || !scene || !h.world->registry.all_of<Transform>(h.e)) return;
          const Transform& t = h.world->registry.get<Transform>(h.e);
          const glm::quat q = glm::quat(glm::radians(glm::vec3(xDeg, yDeg, zDeg))) * t.rotation;
          h.world->setEntityPose(*scene, h.e, t.position, q);
        },
        "impulso", [](const Entidade& h, float x, float y, float z) {
          if (!h.valid()) return;
          if (const RigidBody* b = h.world->registry.try_get<RigidBody>(h.e); b && b->bodyId != ~0u)
            h.world->physics->addImpulse(b->bodyId, glm::vec3(x, y, z));
        },
        "velocidade", [](const Entidade& h) {
          glm::vec3 v(0.0f);
          if (h.valid()) {
            if (const RigidBody* b = h.world->registry.try_get<RigidBody>(h.e); b && b->bodyId != ~0u)
              v = h.world->physics->linearVelocity(b->bodyId);
            else if (const Character* c = h.world->registry.try_get<Character>(h.e); c && c->id != ~0u)
              v = h.world->physics->characterVelocity(c->id);
          }
          return std::make_tuple(v.x, v.y, v.z);
        },
        "ir_para", [](const Entidade& h, float x, float y, float z) {  // IA: anda até o ponto pela navegação
          if (!h.valid()) return;
          if (Character* c = h.world->registry.try_get<Character>(h.e)) {
            c->ai = true;
            c->player = false;
            c->aiTarget = glm::vec3(x, y, z);
            c->aiFollow.clear();
            c->aiHasTarget = true;
            c->aiRepath = 0.0f;
          }
        },
        "seguir", [](const Entidade& h, const Entidade& alvo) {
          if (!h.valid() || !alvo.valid()) return;
          if (Character* c = h.world->registry.try_get<Character>(h.e)) {
            const Name* n = alvo.world->registry.try_get<Name>(alvo.e);
            if (!n) return;
            c->ai = true;
            c->player = false;
            c->aiFollow = n->value;
            c->aiHasTarget = true;
            c->aiRepath = 0.0f;
          }
        },
        "animar", [](const Entidade& h, const std::string& clip, sol::optional<float> fade, sol::optional<bool> loop) {
          return h.valid() && h.world->playAction(h.e, clip, fade.value_or(0.2f), loop.value_or(false));
        },
        "ragdoll", [](const Entidade& h, sol::optional<float> ix, sol::optional<float> iy, sol::optional<float> iz) {
          return h.valid() && h.world->startRagdoll(h.e, glm::vec3(ix.value_or(0.0f), iy.value_or(0.0f), iz.value_or(0.0f)));
        },
        "levantar", [](const Entidade& h) { return h.valid() && h.world->stopRagdoll(h.e); },
        "dormindo", [](const Entidade& h) { return h.valid() && h.world->registry.all_of<Dormant>(h.e); },
        "caido", [](const Entidade& h) {
          const Character* c = h.valid() ? h.world->registry.try_get<Character>(h.e) : nullptr;
          return c && c->ragdoll.active;
        },
        "parar_animacao", [](const Entidade& h, sol::optional<float> fade) {
          if (h.valid()) h.world->stopAction(h.e, fade.value_or(0.2f));
        },
        "osso", [](const Entidade& h, const std::string& bone, sol::this_state s) -> sol::variadic_results {
          sol::variadic_results r;
          glm::vec3 p;
          if (h.valid() && h.world->bonePosition(h.e, bone, p)) {
            r.push_back({s, sol::in_place, p.x});
            r.push_back({s, sol::in_place, p.y});
            r.push_back({s, sol::in_place, p.z});
          } else {
            r.push_back({s, sol::in_place, sol::lua_nil});
          }
          return r;
        },
        "particulas", [](const Entidade& h, sol::optional<bool> on) -> bool {  // liga/desliga; devolve o estado
          if (!h.valid()) return false;
          ParticleSource* ps = h.world->registry.try_get<ParticleSource>(h.e);
          if (!ps) return false;
          if (on) ps->enabled = *on;
          return ps->enabled;
        },
        "rpm", [](const Entidade& h) -> float {  // carro: giro do motor
          if (!h.valid()) return 0.0f;
          const Vehicle* v = h.world->registry.try_get<Vehicle>(h.e);
          return v && v->id != ~0u ? h.world->physics->vehicleRpm(v->id) : 0.0f;
        },
        "marcha", [](const Entidade& h) -> int {  // carro: marcha engatada (-1 = ré, 0 = neutro)
          if (!h.valid()) return 0;
          const Vehicle* v = h.world->registry.try_get<Vehicle>(h.e);
          return v && v->id != ~0u ? h.world->physics->vehicleGear(v->id) : 0;
        },
        "velocidade_frente", [](const Entidade& h) -> float {  // carro: m/s para a frente (negativo = ré)
          if (!h.valid()) return 0.0f;
          const Vehicle* v = h.world->registry.try_get<Vehicle>(h.e);
          return v && v->id != ~0u ? h.world->physics->vehicleForwardSpeed(v->id) : 0.0f;
        },
        "patrulhar", [](const Entidade& h, sol::table pontos) {  // IA: patrulha os pontos {{x,y,z}, ...}
          if (!h.valid()) return;
          if (Character* c = h.world->registry.try_get<Character>(h.e)) {
            c->aiPatrol.clear();
            for (size_t i = 1; i <= pontos.size(); ++i) {
              sol::table p = pontos[i];
              c->aiPatrol.emplace_back(p.get_or(1, 0.0f), p.get_or(2, 0.0f), p.get_or(3, 0.0f));
            }
            c->aiPatrolIdx = 0;
            c->ai = c->aiHasTarget = true;
            c->player = false;
            c->aiRepath = 0.0f;
          }
        },
        "perseguir", [](const Entidade& h, const Entidade& alvo) {
          if (!h.valid() || !alvo.valid()) return;
          Character* c = h.world->registry.try_get<Character>(h.e);
          const Name* n = alvo.world->registry.try_get<Name>(alvo.e);
          if (!c || !n) return;
          c->aiChase = n->value;
          c->ai = c->aiHasTarget = true;
          c->player = false;
        },
        "fugir_de", [](const Entidade& h, const Entidade& alvo) {
          if (!h.valid() || !alvo.valid()) return;
          Character* c = h.world->registry.try_get<Character>(h.e);
          const Name* n = alvo.world->registry.try_get<Name>(alvo.e);
          if (!c || !n) return;
          c->aiFlee = n->value;
          c->ai = c->aiHasTarget = true;
          c->player = false;
        },
        "estado_ia", [](const Entidade& h) -> std::string {
          if (!h.valid()) return "parado";
          const Character* c = h.world->registry.try_get<Character>(h.e);
          return c ? c->aiState : std::string("parado");
        },
        "ve", [](const Entidade& h, const Entidade& outra) {
          if (!h.valid() || !outra.valid()) return false;
          const Character* c = h.world->registry.try_get<Character>(h.e);
          return c && h.world->sees(*c, outra.e);
        },
        "tocar_som", [](const Entidade& h) {
          if (!h.valid() || !h.world->audio) return;
          if (const SoundSource* so = h.world->registry.try_get<SoundSource>(h.e); so && so->handle != ~0u)
            h.world->audio->play(so->handle);
        },
        "parar_som", [](const Entidade& h) {
          if (!h.valid() || !h.world->audio) return;
          if (const SoundSource* so = h.world->registry.try_get<SoundSource>(h.e); so && so->handle != ~0u)
            h.world->audio->stop(so->handle);
        },
        "definir_velocidade", [](const Entidade& h, float x, float y, float z) {
          if (!h.valid()) return;
          if (const RigidBody* b = h.world->registry.try_get<RigidBody>(h.e); b && b->bodyId != ~0u)
            h.world->physics->setLinearVelocity(b->bodyId, glm::vec3(x, y, z));
        });
    lua.set_function("achar", [this](const std::string& name) -> sol::object {
      for (auto e : world.registry.view<Name>())
        if (world.registry.get<Name>(e).value == name) return sol::make_object(lua, Entidade{&world, e});
      return sol::lua_nil;
    });
    lua.set_function("tempo", [this]() { return world.time; });
    lua.set_function("som", [this](const std::string& path, float x, float y, float z, sol::optional<float> volume) {
      if (world.audio) world.audio->playOnce(path, glm::vec3(x, y, z), volume.value_or(1.0f));
    });
    // Interface de jogo: hud.texto / hud.retangulo / hud.barra, chamados no atualizar() de cada quadro.
    auto color = [](const sol::optional<sol::table>& t, glm::vec4 fallback) {
      if (!t) return fallback;
      const sol::table& c = *t;
      return glm::vec4(c.get_or(1, fallback.r), c.get_or(2, fallback.g), c.get_or(3, fallback.b), c.get_or(4, fallback.a));
    };
    sol::table hud = lua.create_named_table("hud");
    hud.set_function("texto", [this, color](const std::string& text, float x, float y, sol::optional<sol::table> opt) {
      HudItem it;
      it.kind = HudItem::Kind::Text;
      it.text = text;
      it.pos = {x, y};
      if (opt) {
        const sol::table& o = *opt;
        it.textSize = o.get_or("tamanho", it.textSize);
        it.color = color(o.get<sol::optional<sol::table>>("cor"), it.color);
        const std::string a = o.get_or<std::string>("alinhar", "esquerda");
        it.align = a == "centro" ? 1 : a == "direita" ? 2 : 0;
      }
      world.hud.push_back(std::move(it));
    });
    hud.set_function("retangulo", [this, color](float x, float y, float w, float h, sol::optional<sol::table> c) {
      HudItem it;
      it.kind = HudItem::Kind::Rect;
      it.pos = {x, y};
      it.size = {w, h};
      it.color = color(c, glm::vec4(0.0f, 0.0f, 0.0f, 0.5f));
      world.hud.push_back(std::move(it));
    });
    hud.set_function("barra", [this, color](float x, float y, float w, float h, float frac, sol::optional<sol::table> c,
                                            sol::optional<sol::table> fundo) {
      HudItem it;
      it.kind = HudItem::Kind::Bar;
      it.pos = {x, y};
      it.size = {w, h};
      it.value = std::clamp(frac, 0.0f, 1.0f);
      it.color = color(c, glm::vec4(0.2f, 0.8f, 0.3f, 1.0f));
      it.back = color(fundo, it.back);
      world.hud.push_back(std::move(it));
    });
    // ---- Controles de interface (modo imediato): mouse, teclado (setas + Enter) e controle (direcional + A) ----
    auto inside = [this](float x, float y, float w, float h) {
      const glm::vec2 m = world.gameInput.mouse;
      return m.x >= x && m.x <= x + w && m.y >= y && m.y <= y + h;
    };
    auto optNum = [](const sol::optional<sol::table>& o, const char* k, float d) { return o ? o->get_or(k, d) : d; };
    auto optColor = [color](const sol::optional<sol::table>& o, const char* k, glm::vec4 d) {
      return o ? color(o->get<sol::optional<sol::table>>(k), d) : d;
    };
    auto label = [this](const std::string& text, float cx, float y, float h, float size, glm::vec4 c, int align) {
      HudItem it;
      it.kind = HudItem::Kind::Text;
      it.text = text;
      it.textSize = size;
      it.pos = {cx, y + 0.5f * (h - 1.15f * size)};
      it.color = c;
      it.align = align;
      world.hud.push_back(std::move(it));
    };
    auto box = [this](float x, float y, float w, float h, glm::vec4 c) {
      HudItem it;
      it.kind = HudItem::Kind::Rect;
      it.pos = {x, y};
      it.size = {w, h};
      it.color = c;
      world.hud.push_back(std::move(it));
    };
    // Um controle novo: índice na ordem do quadro, foco pelo mouse (quando ele se mexe) ou pela navegação.
    auto widget = [this, inside](float x, float y, float w, float h, bool& focused, bool& hovered) {
      UiState& ui = world.ui;
      const int idx = ui.count++;
      hovered = inside(x, y, w, h);
      if (hovered && (ui.mouseMoved || world.gameInput.mousePressed)) ui.hoverFocus = idx;  // vale no próximo quadro
      focused = ui.focus == idx;
      return idx;
    };
    auto confirm = [this]() { return world.gameInput.hit("enter") || world.gameInput.hit("pad_a"); };
    const glm::vec4 kBg(0.08f, 0.09f, 0.12f, 0.82f), kFocus(0.20f, 0.45f, 0.90f, 0.95f), kText(1.0f);
    hud.set_function("botao", [=, this](const std::string& text, float x, float y, float w, float h, sol::optional<sol::table> o) {
      bool focused, hovered;
      widget(x, y, w, h, focused, hovered);
      const bool clicked = (hovered && world.gameInput.mousePressed) || (focused && confirm());
      box(x, y, w, h, focused ? optColor(o, "cor_foco", kFocus) : optColor(o, "cor", kBg));
      label(text, x + 0.5f * w, y, h, optNum(o, "tamanho", 0.5f * h), optColor(o, "cor_texto", kText), 1);
      return clicked;
    });
    hud.set_function("alternar", [=, this](const std::string& text, float x, float y, float w, float h, bool value,
                                     sol::optional<sol::table> o) {
      bool focused, hovered;
      widget(x, y, w, h, focused, hovered);
      if ((hovered && world.gameInput.mousePressed) || (focused && confirm())) value = !value;
      box(x, y, w, h, focused ? optColor(o, "cor_foco", kFocus) : optColor(o, "cor", kBg));
      const float size = optNum(o, "tamanho", 0.5f * h);
      label(text, x + 0.03f * w, y, h, size, optColor(o, "cor_texto", kText), 0);
      label(world.translate(value ? "Sim" : "Não"), x + 0.97f * w, y, h, size, value ? glm::vec4(0.45f, 1.0f, 0.5f, 1.0f) : glm::vec4(1.0f, 0.55f, 0.5f, 1.0f), 2);
      return value;
    });
    hud.set_function("deslizar", [=, this](const std::string& text, float x, float y, float w, float h, float value, float lo,
                                     float hi, sol::optional<sol::table> o) {
      bool focused, hovered;
      const int idx = widget(x, y, w, h, focused, hovered);
      UiState& ui = world.ui;
      const GameInput& in = world.gameInput;
      if (hovered && in.mousePressed) ui.dragging = idx;
      if (ui.dragging == idx && in.mouseDown) value = lo + glm::clamp((in.mouse.x - x) / std::max(w, 1e-4f), 0.0f, 1.0f) * (hi - lo);
      if (focused) {
        const float step = (hi - lo) / 20.0f;
        if (in.hit("direita") || in.hit("pad_direita")) value += step;
        if (in.hit("esquerda") || in.hit("pad_esquerda")) value -= step;
      }
      value = glm::clamp(value, std::min(lo, hi), std::max(lo, hi));
      const float f = hi != lo ? (value - lo) / (hi - lo) : 0.0f;
      box(x, y, w, h, focused ? optColor(o, "cor_foco", kFocus) : optColor(o, "cor", kBg));
      HudItem bar;
      bar.kind = HudItem::Kind::Bar;
      bar.pos = {x + 0.02f * w, y + 0.72f * h};
      bar.size = {0.96f * w, 0.14f * h};
      bar.value = f;
      bar.color = glm::vec4(0.95f, 0.95f, 1.0f, 1.0f);
      bar.back = glm::vec4(1.0f, 1.0f, 1.0f, 0.18f);
      world.hud.push_back(bar);
      const float size = optNum(o, "tamanho", 0.42f * h);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%d%%", int(std::lround(100.0f * f)));
      label(text, x + 0.03f * w, y - 0.12f * h, h, size, optColor(o, "cor_texto", kText), 0);
      label(buf, x + 0.97f * w, y - 0.12f * h, h, size, optColor(o, "cor_texto", kText), 2);
      return value;
    });

    // ---- Entrada ----
    lua.set_function("tecla", [this](const std::string& k) { return world.gameInput.down(k); });
    lua.set_function("tecla_apertada", [this](const std::string& k) { return world.gameInput.hit(k); });
    lua.set_function("mouse", [this]() { return std::make_tuple(world.gameInput.mouse.x, world.gameInput.mouse.y); });
    lua.set_function("clique", [this]() { return world.gameInput.mousePressed; });
    lua.set_function("mouse_segurado", [this]() { return world.gameInput.mouseDown; });
    lua.set_function("controle", [this](const std::string& b) { return world.gameInput.down("pad_" + b); });
    lua.set_function("controle_apertado", [this](const std::string& b) { return world.gameInput.hit("pad_" + b); });
    lua.set_function("eixo", [this](const std::string& a) { return world.gameInput.axis(a); });
    lua.set_function("tem_controle", [this]() { return world.gameInput.pad; });

    // ---- Jogo: pausa, sair, opções ----
    lua.set_function("pausar", [this](sol::optional<bool> on) { world.paused = on.value_or(true); });
    lua.set_function("pausado", [this]() { return world.paused; });
    lua.set_function("sair", [this]() { world.quitRequested = true; });
    lua.set_function("explosao", [this](float x, float y, float z, sol::optional<float> scale) {
      return world.explode(glm::vec3(x, y, z), scale.value_or(1.0f));
    });
    lua.set_function("camera", [this](float x, float y, float z, float ax, float ay, float az, sol::optional<float> fov) {
      world.cameraOverride.active = true;
      world.cameraOverride.position = glm::vec3(x, y, z);
      world.cameraOverride.target = glm::vec3(ax, ay, az);
      world.cameraOverride.fovDeg = fov.value_or(0.0f);
    });
    lua.set_function("camera_jogo", [this]() { world.cameraOverride.active = false; });
    // Idiomas: t("chave", {nome = "Ana"}) troca {nome} no texto traduzido.
    lua.set_function("t", [this](const std::string& key, sol::optional<sol::table> vars, sol::this_state st) {
      std::string text = world.translate(key);
      if (vars) {
        sol::state_view L(st);
        for (auto& kv : *vars) {
          if (!kv.first.is<std::string>()) continue;
          const std::string tag = "{" + kv.first.as<std::string>() + "}";
          const std::string val = L["tostring"](kv.second).get<std::string>();
          for (size_t at = text.find(tag); at != std::string::npos; at = text.find(tag, at + val.size()))
            text.replace(at, tag.size(), val);
        }
      }
      return text;
    });
    lua.set_function("idioma", [this](sol::optional<std::string> code) {
      if (code) world.setLanguage(*code, true);
      return world.language;
    });
    lua.set_function("idiomas", [this](sol::this_state st) {
      sol::state_view L(st);
      sol::table out = L.create_table();
      int i = 1;
      for (const auto& [code, name] : world.languages()) out[i++] = L.create_table_with(1, code, 2, name);
      return out;
    });
    // Mundo aberto: células carregadas e total (e entidades nas células).
    lua.set_function("celulas", [this](sol::this_state st) {
      sol::variadic_results r;
      r.push_back({st, sol::in_place, world.openWorld.loaded});
      r.push_back({st, sol::in_place, uint32_t(world.openWorld.cells.size())});
      return r;
    });
    lua.set_function("altura_terreno", [this](float x, float z) { return world.terrainHeight(x, z); });
    lua.set_function("controle_jogador", [this](sol::optional<bool> on) {
      if (on) world.playerControl = *on;
      return world.playerControl;
    });
    lua.set_function("tempo_real", [this]() { return world.realTime; });
    lua.set_function("volume", [this](sol::optional<float> v) {
      if (!world.audio) return v.value_or(1.0f);
      if (v) world.audio->setMasterVolume(glm::clamp(*v, 0.0f, 1.0f));
      return world.audio->masterVolume();
    });
    lua.set_function("opcao", [this](const std::string& name, sol::object value) {
      float v = value.is<bool>() ? (value.as<bool>() ? 1.0f : 0.0f) : value.is<double>() ? float(value.as<double>()) : 0.0f;
      world.optionRequests.emplace_back(name, v);
    });
    lua.set_function("qualidade", [this](const std::string& preset) { world.qualityRequest = preset; });

    // ---- Salvar e carregar (saves/<nome>.json) ----
    lua.set_function("salvar_jogo", [this](const std::string& name, sol::object data) {
      return world.saveGame(name, data.valid() && data.get_type() != sol::type::lua_nil ? luaToJson(data).dump() : std::string());
    });
    lua.set_function("carregar_jogo", [this](const std::string& name, sol::this_state st) -> sol::object {
      std::string data;
      if (!world.loadGame(name, data)) return sol::make_object(st, sol::lua_nil);
      if (data.empty()) return sol::make_object(st, sol::lua_nil);
      return jsonToLua(nlohmann::json::parse(data), st);
    });
    lua.set_function("existe_jogo", [](const std::string& name) { return World::gameExists(name); });

    // incluir("lib/dialogo.lua"): roda outro arquivo no ambiente do script atual (bibliotecas de jogo). Procura como
    // dado, ao lado do script e em scripts/.
    lua.set_function("incluir", [this](const std::string& file) {
      if (!current) return false;
      std::vector<std::string> tries = {file};
      const std::filesystem::path here = std::filesystem::path(current->path).parent_path();
      tries.push_back((here / file).string());
      tries.push_back((std::filesystem::path("scripts") / file).string());
      for (const std::string& t : tries) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(t, ec)) continue;
        sol::protected_function_result r = lua.safe_script_file(t, current->env, sol::script_pass_on_error);
        if (!r.valid()) {
          sol::error err = r;
          LOG_ERROR("incluir(%s): %s", t.c_str(), err.what());
          return false;
        }
        return true;
      }
      LOG_ERROR("incluir: %s não encontrado", file.c_str());
      return false;
    });
    lua.set_function("log", [](sol::variadic_args args, sol::this_state s) {
      sol::state_view L(s);
      std::ostringstream out;
      for (auto a : args) {
        sol::object o = a;
        out << L["tostring"](o).get<std::string>() << ' ';
      }
      LOG_INFO("[script] %s", out.str().c_str());
    });
  }

  bool run(Instance& inst) {
    Instance* prev = current;
    current = &inst;
    sol::protected_function_result r = lua.safe_script_file(inst.path, inst.env, sol::script_pass_on_error);
    current = prev;
    if (!r.valid()) {
      sol::error err = r;
      LOG_ERROR("Script %s: %s", inst.path.c_str(), err.what());
      inst.failed = true;
      return false;
    }
    inst.failed = false;
    return true;
  }

  template <typename... Args>
  void call(Instance& inst, const char* fn, Args&&... args) {
    sol::object f = inst.env[fn];
    if (f.get_type() != sol::type::function) return;
    sol::protected_function pf = f;
    Instance* prev = current;
    current = &inst;
    sol::protected_function_result r = pf(std::forward<Args>(args)...);
    current = prev;
    if (!r.valid()) {
      sol::error err = r;
      LOG_ERROR("Script %s, %s(): %s", inst.path.c_str(), fn, err.what());
      inst.failed = true;
    }
  }
};

Scripting::Scripting(World& world) : p_(std::make_unique<Impl>(world)) {}
Scripting::~Scripting() = default;

void Scripting::attach(entt::entity e, const std::string& path) {
  Impl::Instance inst;
  inst.e = e;
  inst.path = path;
  inst.env = sol::environment(p_->lua, sol::create, p_->lua.globals());
  p_->stamps[path] = modified(path);
  p_->run(inst);
  p_->instances.push_back(std::move(inst));
}

void Scripting::update(scene::Scene& scene, float dt) {
  p_->scene = &scene;
  // Recarga a quente: a cada 0,5 s confere a data dos arquivos; mudou = roda de novo no mesmo ambiente.
  p_->sinceCheck += dt;
  if (p_->sinceCheck >= 0.5f) {
    p_->sinceCheck = 0.0f;
    p_->world.refreshVisualScripts();
    for (auto& [path, stamp] : p_->stamps) {
      const auto now = modified(path);
      if (now == stamp) continue;
      stamp = now;
      LOG_INFO("Script %s mudou: recarregando", path.c_str());
      for (auto& inst : p_->instances)
        if (inst.path == path) p_->run(inst);
    }
  }
  p_->world.hud.clear();  // a interface é refeita pelos scripts a cada quadro de jogo
  {  // controles de interface: quadro novo (foco por teclado/direcional entre os controles do quadro anterior)
    UiState& ui = p_->world.ui;
    const GameInput& in = p_->world.gameInput;
    ui.prevCount = ui.count;
    ui.count = 0;
    ui.mouseMoved = ui.lastMouse.x >= 0.0f && glm::length(in.mouse - ui.lastMouse) > 1e-4f;
    ui.lastMouse = in.mouse;
    if (!in.mouseDown) ui.dragging = -1;
    if (ui.hoverFocus >= 0) ui.focus = ui.hoverFocus;
    ui.hoverFocus = -1;
    if (ui.prevCount > 0) {
      if (in.hit("baixo") || in.hit("pad_baixo") || in.hit("tab")) ui.focus = (ui.focus + 1) % ui.prevCount;
      if (in.hit("cima") || in.hit("pad_cima")) ui.focus = (ui.focus + ui.prevCount - 1) % ui.prevCount;
      ui.focus = std::min(ui.focus, ui.prevCount - 1);
    } else {
      ui.focus = 0;
    }
  }
  for (auto& inst : p_->instances) {
    if (inst.failed || !p_->world.registry.valid(inst.e)) continue;
    if (p_->world.registry.all_of<Dormant>(inst.e)) continue;  // mundo aberto: célula descarregada
    const Entidade h{&p_->world, inst.e};
    if (!inst.started) {
      inst.started = true;
      p_->call(inst, "iniciar", h);
      if (inst.failed) continue;
    }
    p_->call(inst, "atualizar", h, dt);
  }
  p_->scene = nullptr;
}

int Scripting::failures() const {
  int n = 0;
  for (const auto& inst : p_->instances) n += inst.failed ? 1 : 0;
  return n;
}

}  // namespace game
