#include "game/world.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "scene/camera.h"
#include "scene/mesh.h"
#include "scene/scene.h"

namespace game {

using json = nlohmann::json;

namespace {

glm::vec3 vec3Of(const json& j, glm::vec3 fallback) {
  if (!j.is_array() || j.size() != 3) return fallback;
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

json toJson(glm::vec3 v) { return json::array({v.x, v.y, v.z}); }

// Matriz de desenho: posição, rotação e o tamanho da forma (as malhas base têm meia extensão / raio 1).
glm::mat4 drawMatrix(const Transform& t, const Shape& s) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation);
  switch (s.type) {
    case ShapeType::Box: return glm::scale(m, s.halfExtents);
    case ShapeType::Sphere: return glm::scale(m, glm::vec3(s.radius));
    case ShapeType::Capsule: return glm::scale(m, glm::vec3(s.radius));  // a malha da cápsula já tem a proporção
  }
  return m;
}

// Cápsula de raio 1 com o trecho reto de meia altura `ratio` (= halfHeight / radius): a esfera aberta no equador.
scene::MeshData makeCapsule(float ratio) {
  scene::MeshData m = scene::makeSphere(48, 24);
  for (auto& v : m.vertices) v.position.y += v.position.y >= 0.0f ? ratio : -ratio;
  return m;
}

const char* motionName(Motion m) { return m == Motion::Static ? "estatico" : m == Motion::Dynamic ? "dinamico" : "cinematico"; }
const char* shapeName(ShapeType t) { return t == ShapeType::Box ? "caixa" : t == ShapeType::Sphere ? "esfera" : "capsula"; }

World* gCurrent = nullptr;

// Velocidade inicial lida do arquivo (aplicada ao criar o corpo).
struct InitialVelocity {
  glm::vec3 value;
};

}  // namespace

// O que o arquivo trazia além das entidades: volta igual no save().
struct World::FileData {
  json sun, camera, materials, extra;
};

World::World() : physics(std::make_unique<Physics>()), file_(std::make_unique<FileData>()) {}
World::~World() {
  registry.clear();  // antes da física (os corpos são destruídos com ela)
  if (gCurrent == this) gCurrent = nullptr;
}

bool World::load(const std::string& path, scene::Scene& scene, scene::Camera& camera) {
  std::ifstream f(path);
  if (!f) {
    LOG_ERROR("Cena: não abri %s", path.c_str());
    return false;
  }
  json doc;
  try {
    doc = json::parse(f, nullptr, true, true);  // aceita comentários // e /* */
  } catch (const json::exception& e) {
    LOG_ERROR("Cena %s: JSON inválido (%s)", path.c_str(), e.what());
    return false;
  }
  try {
    // Sol e céu.
    if (doc.contains("sol")) {
      const json& sol = doc["sol"];
      file_->sun = sol;
      const float e = glm::radians(sol.value("elevacao", 40.0f)), a = glm::radians(sol.value("azimute", 135.0f));
      scene.sunDirection = glm::normalize(glm::vec3(std::cos(e) * std::sin(a), std::sin(e), -std::cos(e) * std::cos(a)));
      scene.sunIlluminance = sol.value("iluminancia", scene.sunIlluminance);
    }
    // Câmera inicial.
    if (doc.contains("camera")) {
      const json& c = doc["camera"];
      file_->camera = c;
      camera.position = vec3Of(c.value("posicao", json()), camera.position);
      camera.lookAt(vec3Of(c.value("alvo", json()), camera.position + camera.forward()));
      camera.fovY = glm::radians(c.value("fov", 60.0f));
    }
    // Materiais por nome.
    std::map<std::string, uint32_t> materials;
    if (doc.contains("materiais")) {
      file_->materials = doc["materiais"];
      for (auto& [name, m] : doc["materiais"].items()) {
        scene::Material mat;
        mat.baseColor = glm::vec4(vec3Of(m.value("cor", json()), glm::vec3(0.8f)), 1.0f);
        mat.roughness = m.value("rugosidade", 0.5f);
        mat.metallic = m.value("metalico", 0.0f);
        mat.emissive = vec3Of(m.value("emissao", json()), glm::vec3(0.0f));
        materials[name] = scene.addMaterial(mat);
      }
    }
    if (materials.find("") == materials.end()) materials[""] = scene.addMaterial(scene::Material{});

    // Malhas base (compartilhadas): caixa e esfera de meia extensão / raio 1; uma cápsula por proporção.
    const uint32_t cube = scene.addMesh(scene::makeCube());
    const uint32_t sphere = scene.addMesh(scene::makeSphere(48, 24));
    std::map<int, uint32_t> capsules;

    uint32_t created = 0;
    for (const json& e : doc.value("entidades", json::array())) {
      Shape shape;
      const std::string forma = e.value("forma", "caixa");
      if (forma == "caixa") {
        shape.type = ShapeType::Box;
        shape.halfExtents = vec3Of(e.value("meia_extensao", json()), glm::vec3(0.5f));
      } else if (forma == "esfera") {
        shape.type = ShapeType::Sphere;
        shape.radius = e.value("raio", 0.5f);
      } else if (forma == "capsula") {
        shape.type = ShapeType::Capsule;
        shape.radius = e.value("raio", 0.3f);
        shape.halfHeight = e.value("meia_altura", 0.6f);
      } else {
        LOG_WARN("Cena: forma desconhecida '%s' (use caixa, esfera ou capsula)", forma.c_str());
        continue;
      }
      Transform base;
      base.position = vec3Of(e.value("posicao", json()), glm::vec3(0.0f));
      if (e.contains("rotacao")) {  // quaternion [w, x, y, z]
        const json& q = e["rotacao"];
        base.rotation = glm::normalize(glm::quat(q[0].get<float>(), q[1].get<float>(), q[2].get<float>(), q[3].get<float>()));
      } else if (e.contains("rotacao_graus")) {  // ângulos de Euler em graus (x, y, z)
        base.rotation = glm::quat(glm::radians(vec3Of(e["rotacao_graus"], glm::vec3(0.0f))));
      }
      const std::string matName = e.value("material", "");
      auto mit = materials.find(matName);
      if (mit == materials.end()) {
        LOG_WARN("Cena: material '%s' não existe; usando o padrão", matName.c_str());
        mit = materials.find("");
      }
      uint32_t mesh = cube;
      if (shape.type == ShapeType::Sphere) mesh = sphere;
      if (shape.type == ShapeType::Capsule) {
        const int key = int(std::lround(1000.0f * shape.halfHeight / shape.radius));
        auto c = capsules.find(key);
        if (c == capsules.end()) c = capsules.emplace(key, scene.addMesh(makeCapsule(float(key) / 1000.0f))).first;
        mesh = c->second;
      }
      bool hasBody = e.contains("fisica");
      RigidBody body;
      if (hasBody) {
        const json& fis = e["fisica"];
        const std::string tipo = fis.value("tipo", "estatico");
        body.desc.motion = tipo == "dinamico" ? Motion::Dynamic : tipo == "cinematico" ? Motion::Kinematic : Motion::Static;
        body.desc.mass = fis.value("massa", 0.0f);
        body.desc.friction = fis.value("atrito", body.desc.friction);
        body.desc.restitution = fis.value("quique", body.desc.restitution);
        body.desc.linearDamping = fis.value("amortecimento_linear", body.desc.linearDamping);
        body.desc.angularDamping = fis.value("amortecimento_angular", body.desc.angularDamping);
      }
      const glm::vec3 velocity = vec3Of(e.value("velocidade", json()), glm::vec3(0.0f));

      // "repetir": {"quantidade": [nx, ny, nz], "passo": [dx, dy, dz]} cria uma grade de cópias.
      glm::ivec3 count(1);
      glm::vec3 step(0.0f);
      if (e.contains("repetir")) {
        const glm::vec3 n = vec3Of(e["repetir"].value("quantidade", json()), glm::vec3(1.0f));
        count = glm::max(glm::ivec3(n), glm::ivec3(1));
        step = vec3Of(e["repetir"].value("passo", json()), glm::vec3(0.0f));
      }
      const std::string name = e.value("nome", "");
      for (int z = 0; z < count.z; ++z)
        for (int y = 0; y < count.y; ++y)
          for (int x = 0; x < count.x; ++x) {
            Transform t = base;
            t.position += step * glm::vec3(x, y, z);
            const entt::entity ent = registry.create();
            const bool many = count.x * count.y * count.z > 1;
            if (!name.empty())
              registry.emplace<Name>(ent, many ? name + "_" + std::to_string(x) + "_" + std::to_string(y) + "_" + std::to_string(z) : name);
            registry.emplace<Transform>(ent, t);
            registry.emplace<Shape>(ent, shape);
            registry.emplace<Renderable>(ent, scene.addObject(mesh, drawMatrix(t, shape), mit->second), matName);
            if (hasBody) {
              registry.emplace<RigidBody>(ent, body);
              if (glm::dot(velocity, velocity) > 0.0f) registry.emplace<InitialVelocity>(ent, velocity);
            }
            ++created;
          }
    }
    // Campos que o motor ainda não usa voltam iguais no save().
    for (auto& [k, v] : doc.items())
      if (k != "sol" && k != "camera" && k != "materiais" && k != "entidades") file_->extra[k] = v;
    LOG_INFO("Cena %s: %u entidades", path.c_str(), created);
  } catch (const json::exception& e) {
    LOG_ERROR("Cena %s: campo inválido (%s)", path.c_str(), e.what());
    return false;
  }
  return true;
}

void World::start() {
  auto view = registry.view<Transform, Shape, RigidBody>();
  for (auto ent : view) {
    auto [t, s, b] = view.get<Transform, Shape, RigidBody>(ent);
    switch (s.type) {
      case ShapeType::Box: b.bodyId = physics->addBox(s.halfExtents, t.position, t.rotation, b.desc); break;
      case ShapeType::Sphere: b.bodyId = physics->addSphere(s.radius, t.position, t.rotation, b.desc); break;
      case ShapeType::Capsule: b.bodyId = physics->addCapsule(s.halfHeight, s.radius, t.position, t.rotation, b.desc); break;
    }
    if (b.bodyId != ~0u && registry.all_of<InitialVelocity>(ent))
      physics->setLinearVelocity(b.bodyId, registry.get<InitialVelocity>(ent).value);
  }
  physics->optimizeBroadphase();
  LOG_INFO("Física: %u corpos (%u ativos)", physics->bodyCount(), physics->activeBodyCount());
}

void World::update(scene::Scene& scene, float dt) {
  if (dt <= 0.0f) return;
  physics->update(dt);
  // Só os corpos que se movem mudam a pose; parados (dormindo) não reenviam nada à GPU.
  auto view = registry.view<Transform, Shape, RigidBody, Renderable>();
  for (auto ent : view) {
    auto [t, s, b, r] = view.get<Transform, Shape, RigidBody, Renderable>(ent);
    if (b.bodyId == ~0u || b.desc.motion == Motion::Static || !physics->isActive(b.bodyId)) continue;
    physics->pose(b.bodyId, t.position, t.rotation);
    scene.setTransform(r.sceneObject, drawMatrix(t, s));
  }
}

bool World::save(const std::string& path) const {
  json doc = file_->extra.is_object() ? file_->extra : json::object();
  if (!file_->sun.is_null()) doc["sol"] = file_->sun;
  if (!file_->camera.is_null()) doc["camera"] = file_->camera;
  if (!file_->materials.is_null()) doc["materiais"] = file_->materials;
  json ents = json::array();
  // Ordem de criação (o registro guarda as entidades na ordem inversa).
  std::vector<entt::entity> order;
  for (entt::entity ent : registry.view<Transform>()) order.push_back(ent);
  std::sort(order.begin(), order.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
  for (entt::entity ent : order) {
    if (!registry.valid(ent) || !registry.all_of<Transform, Shape>(ent)) continue;
    const Transform& t = registry.get<Transform>(ent);
    const Shape& s = registry.get<Shape>(ent);
    json e;
    if (const Name* n = registry.try_get<Name>(ent)) e["nome"] = n->value;
    e["forma"] = shapeName(s.type);
    if (s.type == ShapeType::Box) e["meia_extensao"] = toJson(s.halfExtents);
    else e["raio"] = s.radius;
    if (s.type == ShapeType::Capsule) e["meia_altura"] = s.halfHeight;
    e["posicao"] = toJson(t.position);
    e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
    if (const Renderable* r = registry.try_get<Renderable>(ent); r && !r->material.empty()) e["material"] = r->material;
    if (const RigidBody* b = registry.try_get<RigidBody>(ent)) {
      json fis;
      fis["tipo"] = motionName(b->desc.motion);
      if (b->desc.mass > 0.0f) fis["massa"] = b->desc.mass;
      fis["atrito"] = b->desc.friction;
      fis["quique"] = b->desc.restitution;
      fis["amortecimento_linear"] = b->desc.linearDamping;
      fis["amortecimento_angular"] = b->desc.angularDamping;
      e["fisica"] = fis;
      if (b->desc.motion == Motion::Dynamic && b->bodyId != ~0u) e["velocidade"] = toJson(physics->linearVelocity(b->bodyId));
    }
    ents.push_back(e);
  }
  doc["entidades"] = ents;
  std::ofstream f(path);
  if (!f) {
    LOG_ERROR("Cena: não consegui gravar %s", path.c_str());
    return false;
  }
  f << doc.dump(2) << '\n';
  LOG_INFO("Cena gravada em %s (%zu entidades, %llu passos de física)", path.c_str(), ents.size(),
           (unsigned long long)physics->steps());
  return bool(f);
}

bool loadWorldScene(const std::string& path, scene::Scene& scene, scene::Camera& camera) {
  auto world = std::make_shared<World>();
  if (!world->load(path, scene, camera)) return false;
  world->start();
  gCurrent = world.get();
  scene.addAnimator([world](scene::Scene& s, float, float dt) { world->update(s, dt); });
  return true;
}

World* currentWorld() { return gCurrent; }

}  // namespace game
