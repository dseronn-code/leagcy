#include "game/scripting.h"

#include <filesystem>
#include <map>
#include <sstream>
#include <vector>

#define SOL_LUA_VERSION 504
#include <sol/sol.hpp>

#include <glm/gtc/quaternion.hpp>

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
  std::map<std::string, std::filesystem::file_time_type> stamps;
  float sinceCheck = 0.0f;

  explicit Impl(World& w) : world(w) {
    lua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
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
    sol::protected_function_result r = lua.safe_script_file(inst.path, inst.env, sol::script_pass_on_error);
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
    sol::protected_function_result r = pf(std::forward<Args>(args)...);
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
    for (auto& [path, stamp] : p_->stamps) {
      const auto now = modified(path);
      if (now == stamp) continue;
      stamp = now;
      LOG_INFO("Script %s mudou: recarregando", path.c_str());
      for (auto& inst : p_->instances)
        if (inst.path == path) p_->run(inst);
    }
  }
  for (auto& inst : p_->instances) {
    if (inst.failed || !p_->world.registry.valid(inst.e)) continue;
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
