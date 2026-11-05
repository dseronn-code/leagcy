#include "game/world.h"
#include "game/navigation.h"
#include "game/scripting.h"

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

// Cilindro de raio 1 com eixo em Y de -1 a 1 (rodas: escala (raio, meia largura, raio)).
scene::MeshData makeCylinder(int segments = 32) {
  scene::MeshData m;
  auto vert = [&](glm::vec3 p, glm::vec3 n, glm::vec2 uv) {
    scene::Vertex v{};
    v.position = p;
    v.normal = n;
    v.uv = uv;
    v.tangent = glm::vec4(1, 0, 0, 1);
    m.vertices.push_back(v);
    return uint32_t(m.vertices.size() - 1);
  };
  for (int i = 0; i < segments; ++i) {  // lateral
    const float a0 = 6.2831853f * float(i) / float(segments), a1 = 6.2831853f * float(i + 1) / float(segments);
    const glm::vec3 n0(std::cos(a0), 0, std::sin(a0)), n1(std::cos(a1), 0, std::sin(a1));
    const uint32_t a = vert(n0 + glm::vec3(0, -1, 0), n0, {0, 0}), b = vert(n1 + glm::vec3(0, -1, 0), n1, {1, 0});
    const uint32_t c = vert(n1 + glm::vec3(0, 1, 0), n1, {1, 1}), d = vert(n0 + glm::vec3(0, 1, 0), n0, {0, 1});
    m.indices.insert(m.indices.end(), {a, c, b, a, d, c});
    for (float y : {-1.0f, 1.0f}) {  // tampas
      const glm::vec3 n(0, y, 0);
      const uint32_t o = vert(n, n, {0.5f, 0.5f}), p0 = vert(n0 + n, n, {0, 0}), p1 = vert(n1 + n, n, {1, 0});
      if (y > 0) m.indices.insert(m.indices.end(), {o, p1, p0});
      else m.indices.insert(m.indices.end(), {o, p0, p1});
    }
  }
  return m;
}

const char* motionName(Motion m) { return m == Motion::Static ? "estatico" : m == Motion::Dynamic ? "dinamico" : "cinematico"; }
const char* shapeName(ShapeType t) { return t == ShapeType::Box ? "caixa" : t == ShapeType::Sphere ? "esfera" : "capsula"; }

World* gCurrent = nullptr;
bool gAudioOffline = false;

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
  scripting.reset();  // o Lua guarda referências às entidades
  navigation.reset();
  audio.reset();
  registry.clear();   // antes da física (os corpos são destruídos com ela)
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
    uint32_t cylinder = ~0u;  // rodas (criado no primeiro veículo)

    uint32_t created = 0;
    loadedScene_ = &scene;
    const std::string sceneDir = [&] {
      const size_t slash = path.find_last_of("/\\");
      return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    }();
    for (const json& e : doc.value("entidades", json::array())) {
      if (e.contains("modelo")) {  // glTF: caminho relativo à pasta do motor ou à pasta da cena
        Model m;
        m.path = e["modelo"].get<std::string>();
        m.scale = e.value("escala", 1.0f);
        Transform t;
        t.position = vec3Of(e.value("posicao", json()), glm::vec3(0.0f));
        if (e.contains("rotacao_graus")) t.rotation = glm::quat(glm::radians(vec3Of(e["rotacao_graus"], glm::vec3(0.0f))));
        if (e.contains("rotacao")) {
          const json& q = e["rotacao"];
          t.rotation = glm::normalize(glm::quat(q[0].get<float>(), q[1].get<float>(), q[2].get<float>(), q[3].get<float>()));
        }
        std::string file = m.path;
        if (!std::ifstream(file) && std::ifstream(sceneDir + file)) file = sceneDir + file;
        const glm::mat4 root = glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation) *
                               glm::scale(glm::mat4(1.0f), glm::vec3(m.scale));
        m.firstObject = uint32_t(scene.objects.size());
        if (!scene::loadGltf(scene, file, root)) {
          LOG_WARN("Cena: não carreguei o modelo %s", file.c_str());
          continue;
        }
        m.objectCount = uint32_t(scene.objects.size()) - m.firstObject;
        if (e.contains("fisica")) {
          m.meshCollider = e["fisica"].value("colisao", "malha") == "malha";
          m.friction = e["fisica"].value("atrito", m.friction);
        }
        const entt::entity ent = registry.create();
        if (e.contains("nome")) registry.emplace<Name>(ent, e["nome"].get<std::string>());
        registry.emplace<Transform>(ent, t);
        registry.emplace<Model>(ent, m);
        ++created;
        continue;
      }
      Shape shape;
      const std::string forma = e.contains("personagem") ? "personagem" : e.value("forma", "caixa");
      std::unique_ptr<Character> character;
      std::unique_ptr<Vehicle> vehicle;
      if (e.contains("veiculo")) {  // chassi = caixa do tamanho do carro; a física vem do VehicleDesc
        const json& vj = e["veiculo"];
        vehicle = std::make_unique<Vehicle>();
        VehicleDesc& d = vehicle->desc;
        d.length = vj.value("comprimento", d.length);
        d.width = vj.value("largura", d.width);
        d.height = vj.value("altura", d.height);
        d.mass = vj.value("massa", d.mass);
        d.wheelRadius = vj.value("raio_roda", d.wheelRadius);
        d.wheelWidth = vj.value("largura_roda", d.wheelWidth);
        d.maxSteerDeg = vj.value("direcao_maxima", d.maxSteerDeg);
        d.engineTorque = vj.value("torque", d.engineTorque);
        d.maxRpm = vj.value("rpm_maximo", d.maxRpm);
        d.rearDrive = vj.value("tracao", "dianteira") == "traseira";
        vehicle->wheelMaterial = vj.value("material_roda", "");
        for (const json& k : vj.value("roteiro", json::array())) {
          DriveKey key;
          key.time = k.value("t", 0.0f);
          key.throttle = k.value("acelerar", 0.0f);
          key.steer = k.value("virar", 0.0f);
          key.brake = k.value("freio", 0.0f);
          key.handBrake = k.value("freio_mao", 0.0f);
          vehicle->script.push_back(key);
        }
        vehicle->player = vj.value("controle", vehicle->script.empty() ? "jogador" : "roteiro") == "jogador";
      }
      if (vehicle) {
        shape.type = ShapeType::Box;
        shape.halfExtents = 0.5f * glm::vec3(vehicle->desc.width, vehicle->desc.height, vehicle->desc.length);
      } else if (e.contains("personagem")) {  // cápsula em pé com os pés em "posicao"
        const json& pj = e["personagem"];
        character = std::make_unique<Character>();
        Character& c = *character;
        c.desc.height = pj.value("altura", c.desc.height);
        c.desc.radius = pj.value("raio", c.desc.radius);
        c.desc.mass = pj.value("massa", c.desc.mass);
        c.desc.maxSlopeDeg = pj.value("rampa_maxima", c.desc.maxSlopeDeg);
        c.desc.stepHeight = pj.value("degrau_maximo", c.desc.stepHeight);
        c.walkSpeed = pj.value("velocidade", c.walkSpeed);
        c.runSpeed = pj.value("velocidade_correndo", c.runSpeed);
        c.jumpSpeed = pj.value("pulo", c.jumpSpeed);
        c.player = pj.value("controle", "jogador") == "jogador";
        if (pj.value("controle", "") == "ia") {
          c.ai = true;
          c.player = false;
          if (pj.contains("ia")) {
            const json& ia = pj["ia"];
            if (ia.contains("alvo")) {
              c.aiTarget = vec3Of(ia["alvo"], glm::vec3(0.0f));
              c.aiHasTarget = true;
            }
            c.aiFollow = ia.value("seguir", "");
            if (!c.aiFollow.empty()) c.aiHasTarget = true;
            c.aiStopDistance = ia.value("parar_a", c.aiStopDistance);
          }
        }
        for (const json& k : pj.value("roteiro", json::array())) {
          ScriptKey key;
          key.time = k.value("t", 0.0f);
          const glm::vec3 m = vec3Of(k.value("andar", json()), glm::vec3(0.0f));
          key.move = glm::dot(m, m) > 0.0f ? glm::normalize(glm::vec3(m.x, 0.0f, m.z)) : glm::vec3(0.0f);
          key.jump = k.value("pular", false);
          key.run = k.value("correr", false);
          c.script.push_back(key);
        }
        if (!c.script.empty() && !c.ai) c.player = pj.value("controle", "roteiro") == "jogador";
        shape.type = ShapeType::Capsule;
        shape.radius = c.desc.radius;
        shape.halfHeight = std::max(0.5f * c.desc.height - c.desc.radius, 0.05f);
      } else if (forma == "caixa") {
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
      if (character) base.position.y += 0.5f * character->desc.height;  // arquivo: pés; Transform: centro
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
      std::unique_ptr<SoundSource> sound;
      if (e.contains("som")) {
        const json& sj = e["som"];
        sound = std::make_unique<SoundSource>();
        sound->path = sj.value("arquivo", "");
        if (!sound->path.empty() && !std::ifstream(sound->path) && std::ifstream(sceneDir + sound->path)) sound->path = sceneDir + sound->path;
        sound->desc.volume = sj.value("volume", 1.0f);
        sound->desc.loop = sj.value("laco", false);
        sound->desc.spatial = sj.value("espacial", true);
        sound->desc.minDistance = sj.value("dist_min", 1.0f);
        sound->desc.maxDistance = sj.value("dist_max", 100.0f);
        sound->desc.rolloff = sj.value("queda", 1.0f);
        sound->autoplay = sj.value("tocar", true);
      }
      std::string script = e.value("script", "");
      if (!script.empty() && !std::ifstream(script) && std::ifstream(sceneDir + script)) script = sceneDir + script;

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
            if (character) registry.emplace<Character>(ent, *character);
            if (vehicle) {
              Vehicle v = *vehicle;
              auto wm = materials.find(v.wheelMaterial);
              if (wm == materials.end()) wm = materials.find("");
              if (cylinder == ~0u) cylinder = scene.addMesh(makeCylinder());
              for (uint32_t& w : v.wheelObjects) w = scene.addObject(cylinder, glm::mat4(1.0f), wm->second);
              registry.emplace<Vehicle>(ent, v);
            }
            if (!script.empty()) registry.emplace<Script>(ent, script);
            if (sound) registry.emplace<SoundSource>(ent, *sound);
            if (hasBody && !character && !vehicle) {
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
  // Modelos com colisão pela malha: os triângulos de todos os objetos do modelo, já no mundo, num corpo só.
  for (auto ent : registry.view<Model>()) {
    Model& m = registry.get<Model>(ent);
    if (!m.meshCollider || !loadedScene_) continue;
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> idx;
    for (uint32_t o = m.firstObject; o < m.firstObject + m.objectCount; ++o) {
      const scene::Object& obj = loadedScene_->objects[o];
      const scene::MeshData& mesh = loadedScene_->cpuMesh(obj.mesh);
      if (mesh.skinModel >= 0 || mesh.clothWidth > 0 || mesh.hairStrands > 0) continue;  // deformáveis: fora
      const uint32_t base = uint32_t(verts.size());
      for (const scene::Vertex& v : mesh.vertices) verts.push_back(glm::vec3(obj.transform * glm::vec4(v.position, 1.0f)));
      for (uint32_t i : mesh.indices) idx.push_back(base + i);
    }
    if (!idx.empty()) {
      m.bodyId = physics->addStaticMesh(verts, idx, m.friction);
      LOG_INFO("Física: colisão de %s com %zu triângulos", m.path.c_str(), idx.size() / 3);
    }
  }
  audio = std::make_unique<Audio>(gAudioOffline);
  for (auto ent : registry.view<SoundSource, Transform>()) {
    SoundSource& snd = registry.get<SoundSource>(ent);
    snd.handle = audio->create(snd.path, snd.desc, registry.get<Transform>(ent).position);
    if (snd.autoplay) audio->play(snd.handle);
  }
  if (!registry.view<Script>().empty()) {
    scripting = std::make_unique<Scripting>(*this);
    for (auto ent : registry.view<Script>()) scripting->attach(ent, registry.get<Script>(ent).path);
  }
  for (auto ent : registry.view<Transform, Vehicle>()) {
    const Transform& t = registry.get<Transform>(ent);
    Vehicle& v = registry.get<Vehicle>(ent);
    v.id = physics->addVehicle(v.desc, t.position, t.rotation);
  }
  bool anyAi = false;
  for (auto ent : registry.view<Character>()) anyAi |= registry.get<Character>(ent).ai;
  if (anyAi) {
    // Cenário parado para a navegação: caixas estáticas e modelos com colisão pela malha.
    std::vector<glm::vec3> verts;
    std::vector<uint32_t> idx;
    for (auto ent : registry.view<Transform, Shape, RigidBody>()) {
      if (registry.get<RigidBody>(ent).desc.motion != Motion::Static) continue;
      const Shape& sh = registry.get<Shape>(ent);
      if (sh.type != ShapeType::Box) continue;
      const glm::mat4 m = drawMatrix(registry.get<Transform>(ent), sh);
      const uint32_t base = uint32_t(verts.size());
      for (int c = 0; c < 8; ++c)
        verts.push_back(glm::vec3(m * glm::vec4((c & 1) ? 1.0f : -1.0f, (c & 2) ? 1.0f : -1.0f, (c & 4) ? 1.0f : -1.0f, 1.0f)));
      const uint32_t faces[12][3] = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                                     {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
      for (auto& f : faces)
        for (uint32_t k : f) idx.push_back(base + k);
    }
    for (auto ent : registry.view<Model>()) {
      const Model& m = registry.get<Model>(ent);
      if (!m.meshCollider || !loadedScene_) continue;
      for (uint32_t o = m.firstObject; o < m.firstObject + m.objectCount; ++o) {
        const scene::Object& obj = loadedScene_->objects[o];
        const scene::MeshData& mesh = loadedScene_->cpuMesh(obj.mesh);
        if (mesh.skinModel >= 0 || mesh.clothWidth > 0 || mesh.hairStrands > 0) continue;
        const uint32_t base = uint32_t(verts.size());
        for (const scene::Vertex& v : mesh.vertices) verts.push_back(glm::vec3(obj.transform * glm::vec4(v.position, 1.0f)));
        for (uint32_t i : mesh.indices) idx.push_back(base + i);
      }
    }
    navigation = std::make_unique<Navigation>();
    NavSettings ns;
    for (auto ent : registry.view<Character>())
      if (const Character& c = registry.get<Character>(ent); c.ai) {
        ns.agentHeight = c.desc.height;
        ns.agentRadius = c.desc.radius;
        ns.agentClimb = c.desc.stepHeight;
        ns.agentMaxSlopeDeg = c.desc.maxSlopeDeg;
        break;
      }
    if (!navigation->build(verts, idx, ns)) navigation.reset();
  }
  auto chars = registry.view<Transform, Character>();
  for (auto ent : chars) {
    auto [t, c] = chars.get<Transform, Character>(ent);
    c.id = physics->addCharacter(c.desc, t.position - glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f));
  }
  physics->optimizeBroadphase();
  LOG_INFO("Física: %u corpos (%u ativos), %zu personagens", physics->bodyCount(), physics->activeBodyCount(),
           size_t(chars.size_hint()));
}

void World::update(scene::Scene& scene, float dt) {
  if (audio && dt > 0.0f) audio->mix(dt);  // offline: a mixagem anda junto com o tempo do quadro
  if (dt <= 0.0f || !simulate) return;
  time += dt;
  if (scripting) scripting->update(scene, dt);
  // Personagens: velocidade desejada pela entrada do jogador ou pelo roteiro.
  for (auto ent : registry.view<Character>()) {
    Character& c = registry.get<Character>(ent);
    if (c.id == ~0u) continue;
    glm::vec3 dir(0.0f);
    bool jump = false, run = false;
    if (c.player) {
      // Frente = para onde a câmera olha, no plano do chão.
      const glm::vec3 fwd(std::cos(input_.cameraYaw), 0.0f, std::sin(input_.cameraYaw));
      const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
      dir = fwd * input_.move.y + right * input_.move.x;
      if (glm::dot(dir, dir) > 1.0f) dir = glm::normalize(dir);
      jump = input_.jump;
      run = input_.run;
    } else if (c.ai && c.aiHasTarget && navigation) {
      const glm::vec3 feet = physics->characterFeet(c.id);
      glm::vec3 target = c.aiTarget;
      if (!c.aiFollow.empty()) {
        for (auto other : registry.view<Name, Transform>())
          if (registry.get<Name>(other).value == c.aiFollow) {
            target = registry.get<Transform>(other).position;
            if (const Character* oc = registry.try_get<Character>(other)) target.y -= 0.5f * oc->desc.height;
          }
      }
      // Caminho recalculado a cada 0,5 s (o alvo pode andar) ou quando acabou.
      c.aiRepath -= dt;
      if (c.aiRepath <= 0.0f || c.aiNext >= c.aiPath.size()) {
        c.aiRepath = 0.5f;
        navigation->findPath(feet, target, c.aiPath);
        c.aiNext = c.aiPath.size() > 1 ? 1 : c.aiPath.size();
      }
      const glm::vec2 toTarget(target.x - feet.x, target.z - feet.z);
      if (glm::length(toTarget) > c.aiStopDistance) {
        while (c.aiNext < c.aiPath.size() &&
               glm::length(glm::vec2(c.aiPath[c.aiNext].x - feet.x, c.aiPath[c.aiNext].z - feet.z)) < 0.25f)
          ++c.aiNext;
        if (c.aiNext < c.aiPath.size()) {
          const glm::vec3 d(c.aiPath[c.aiNext].x - feet.x, 0.0f, c.aiPath[c.aiNext].z - feet.z);
          if (glm::length(d) > 1e-3f) dir = glm::normalize(d);
        }
      }
    } else if (!c.script.empty()) {
      size_t k = 0;
      while (k + 1 < c.script.size() && c.script[k + 1].time <= time) ++k;
      if (c.script[k].time <= time) {
        dir = c.script[k].move;
        run = c.script[k].run;
        if (c.script[k].jump && c.scriptJumped <= k) {
          jump = true;
          c.scriptJumped = k + 1;
        }
      }
    }
    physics->moveCharacter(c.id, dir * (run ? c.runSpeed : c.walkSpeed), jump, c.jumpSpeed);
  }
  for (auto ent : registry.view<Vehicle>()) {
    Vehicle& v = registry.get<Vehicle>(ent);
    if (v.id == ~0u) continue;
    float throttle = 0.0f, steer = 0.0f, brake = 0.0f, hand = 0.0f;
    if (v.player) {
      // W acelera, S freia e depois dá ré, A/D viram, espaço puxa o freio de mão.
      throttle = input_.move.y;
      steer = input_.move.x;
      hand = input_.jump ? 1.0f : 0.0f;
      const float speed = physics->vehicleForwardSpeed(v.id);
      if (throttle != 0.0f && speed * throttle < -1.0f) {  // pedal contra o movimento: freia
        brake = 1.0f;
        throttle = 0.0f;
      }
    } else if (!v.script.empty()) {
      size_t k = 0;
      while (k + 1 < v.script.size() && v.script[k + 1].time <= time) ++k;
      if (v.script[k].time <= time) {
        throttle = v.script[k].throttle;
        steer = v.script[k].steer;
        brake = v.script[k].brake;
        hand = v.script[k].handBrake;
      }
    }
    physics->driveVehicle(v.id, throttle, steer, brake, hand);
  }
  physics->update(dt);
  for (auto ent : registry.view<Vehicle, Transform, Shape, Renderable>()) {  // chassi e rodas
    const Vehicle& v = registry.get<Vehicle>(ent);
    if (v.id == ~0u) continue;
    Transform& t = registry.get<Transform>(ent);
    physics->pose(physics->vehicleBody(v.id), t.position, t.rotation);
    scene.setTransform(registry.get<Renderable>(ent).sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
    for (int w = 0; w < 4; ++w)
      scene.setTransform(v.wheelObjects[w], physics->wheelMatrix(v.id, w) *
                                                glm::scale(glm::mat4(1.0f), glm::vec3(v.desc.wheelRadius, 0.5f * v.desc.wheelWidth, v.desc.wheelRadius)));
  }
  for (auto ent : registry.view<SoundSource, Transform>()) {  // sons seguem as entidades
    const SoundSource& snd = registry.get<SoundSource>(ent);
    if (snd.handle != ~0u) audio->setPosition(snd.handle, registry.get<Transform>(ent).position);
  }
  for (auto ent : registry.view<Transform, Shape, Character, Renderable>()) {
    auto& t = registry.get<Transform>(ent);
    const Character& c = registry.get<Character>(ent);
    if (c.id == ~0u) continue;
    t.position = physics->characterFeet(c.id) + glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
    scene.setTransform(registry.get<Renderable>(ent).sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
  }
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
    if (const Model* m = registry.try_get<Model>(ent)) {
      const Transform& t = registry.get<Transform>(ent);
      json e;
      if (const Name* n = registry.try_get<Name>(ent)) e["nome"] = n->value;
      e["modelo"] = m->path;
      e["escala"] = m->scale;
      e["posicao"] = toJson(t.position);
      e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
      if (m->meshCollider) e["fisica"] = {{"tipo", "estatico"}, {"colisao", "malha"}, {"atrito", m->friction}};
      ents.push_back(e);
      continue;
    }
    if (!registry.valid(ent) || !registry.all_of<Transform, Shape>(ent)) continue;
    const Transform& t = registry.get<Transform>(ent);
    const Shape& s = registry.get<Shape>(ent);
    json e;
    if (const Name* n = registry.try_get<Name>(ent)) e["nome"] = n->value;
    if (const Vehicle* v = registry.try_get<Vehicle>(ent)) {
      const VehicleDesc& d = v->desc;
      json vj = {{"comprimento", d.length}, {"largura", d.width}, {"altura", d.height}, {"massa", d.mass},
                 {"raio_roda", d.wheelRadius}, {"largura_roda", d.wheelWidth}, {"direcao_maxima", d.maxSteerDeg},
                 {"torque", d.engineTorque}, {"rpm_maximo", d.maxRpm}, {"tracao", d.rearDrive ? "traseira" : "dianteira"},
                 {"controle", v->player ? "jogador" : "roteiro"}};
      if (!v->wheelMaterial.empty()) vj["material_roda"] = v->wheelMaterial;
      if (!v->script.empty()) {
        json r = json::array();
        for (const DriveKey& k : v->script)
          r.push_back({{"t", k.time}, {"acelerar", k.throttle}, {"virar", k.steer}, {"freio", k.brake}, {"freio_mao", k.handBrake}});
        vj["roteiro"] = r;
      }
      e["veiculo"] = vj;
      e["posicao"] = toJson(t.position);
      e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
      if (v->id != ~0u) e["velocidade"] = toJson(physics->linearVelocity(physics->vehicleBody(v->id)));
      if (const Renderable* r = registry.try_get<Renderable>(ent); r && !r->material.empty()) e["material"] = r->material;
      ents.push_back(e);
      continue;
    }
    if (const Character* c = registry.try_get<Character>(ent)) {
      json pj;
      pj["altura"] = c->desc.height;
      pj["raio"] = c->desc.radius;
      pj["massa"] = c->desc.mass;
      pj["rampa_maxima"] = c->desc.maxSlopeDeg;
      pj["degrau_maximo"] = c->desc.stepHeight;
      pj["velocidade"] = c->walkSpeed;
      pj["velocidade_correndo"] = c->runSpeed;
      pj["pulo"] = c->jumpSpeed;
      pj["controle"] = c->ai ? "ia" : c->player ? "jogador" : "roteiro";
      if (c->ai) {
        json ia;
        if (!c->aiFollow.empty()) ia["seguir"] = c->aiFollow;
        else if (c->aiHasTarget) ia["alvo"] = toJson(c->aiTarget);
        ia["parar_a"] = c->aiStopDistance;
        pj["ia"] = ia;
      }
      if (!c->script.empty()) {
        json r = json::array();
        for (const ScriptKey& k : c->script)
          r.push_back({{"t", k.time}, {"andar", toJson(k.move)}, {"pular", k.jump}, {"correr", k.run}});
        pj["roteiro"] = r;
      }
      e["personagem"] = pj;
      e["posicao"] = toJson(t.position - glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f));
      if (c->id != ~0u) e["velocidade"] = toJson(physics->characterVelocity(c->id));
      if (const Renderable* r = registry.try_get<Renderable>(ent); r && !r->material.empty()) e["material"] = r->material;
      ents.push_back(e);
      continue;
    }
    e["forma"] = shapeName(s.type);
    if (s.type == ShapeType::Box) e["meia_extensao"] = toJson(s.halfExtents);
    else e["raio"] = s.radius;
    if (s.type == ShapeType::Capsule) e["meia_altura"] = s.halfHeight;
    e["posicao"] = toJson(t.position);
    e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
    if (const Renderable* r = registry.try_get<Renderable>(ent); r && !r->material.empty()) e["material"] = r->material;
    if (const Script* sc = registry.try_get<Script>(ent)) e["script"] = sc->path;
    if (const SoundSource* so = registry.try_get<SoundSource>(ent))
      e["som"] = {{"arquivo", so->path}, {"volume", so->desc.volume}, {"laco", so->desc.loop}, {"espacial", so->desc.spatial},
                  {"dist_min", so->desc.minDistance}, {"dist_max", so->desc.maxDistance}, {"queda", so->desc.rolloff},
                  {"tocar", so->autoplay}};
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

bool World::playerVehicle(glm::vec3& position, glm::vec3& forward) const {
  for (auto ent : registry.view<Vehicle, Transform>()) {
    const Vehicle& v = registry.get<Vehicle>(ent);
    if (!v.player || v.id == ~0u) continue;
    const Transform& t = registry.get<Transform>(ent);
    position = t.position;
    forward = t.rotation * glm::vec3(0.0f, 0.0f, 1.0f);
    return true;
  }
  return false;
}
void setAudioOffline(bool offline) { gAudioOffline = offline; }

entt::entity World::entityOfBody(uint32_t body) const {
  if (body == ~0u) return entt::null;
  for (auto ent : registry.view<RigidBody>())
    if (registry.get<RigidBody>(ent).bodyId == body) return ent;
  for (auto ent : registry.view<Model>())
    if (registry.get<Model>(ent).bodyId == body) return ent;
  return entt::null;
}

entt::entity World::addPrimitive(ShapeType type, glm::vec3 position, Motion motion, glm::vec3 halfExtents) {
  const entt::entity e = registry.create();
  Shape s;
  s.type = type;
  s.halfExtents = halfExtents;
  s.radius = 0.5f;
  Transform t;
  t.position = position;
  const char* base = type == ShapeType::Box ? (motion == Motion::Static ? "parede" : "caixa") : "esfera";
  registry.emplace<Name>(e, std::string(base) + "_" + std::to_string(entt::to_integral(e)));
  registry.emplace<Transform>(e, t);
  registry.emplace<Shape>(e, s);
  registry.emplace<Renderable>(e, ~0u, std::string());
  RigidBody b;
  b.desc.motion = motion;
  registry.emplace<RigidBody>(e, b);
  return e;
}

entt::entity World::duplicate(entt::entity src) {
  if (!registry.valid(src) || !registry.all_of<Transform, Shape>(src) || registry.all_of<Character>(src)) return entt::null;
  const entt::entity e = registry.create();
  Transform t = registry.get<Transform>(src);
  t.position.x += 1.0f;
  registry.emplace<Transform>(e, t);
  registry.emplace<Shape>(e, registry.get<Shape>(src));
  if (const Name* n = registry.try_get<Name>(src)) registry.emplace<Name>(e, n->value + "_copia");
  if (const Renderable* r = registry.try_get<Renderable>(src)) registry.emplace<Renderable>(e, ~0u, r->material);
  if (const RigidBody* b = registry.try_get<RigidBody>(src)) registry.emplace<RigidBody>(e, RigidBody{b->desc, ~0u});
  return e;
}

void World::snapshot() {
  snapshot_.clear();
  for (auto e : registry.view<Transform>()) snapshot_.emplace_back(e, registry.get<Transform>(e));
  snapshotTime_ = time;
}

void World::restore(scene::Scene& scene) {
  for (auto& [e, t] : snapshot_) {
    if (!registry.valid(e)) continue;
    if (Character* c = registry.try_get<Character>(e); c && c->id != ~0u) {
      physics->setCharacterFeet(c->id, t.position - glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f));
      registry.get<Transform>(e) = t;
      if (const Renderable* r = registry.try_get<Renderable>(e); r && registry.all_of<Shape>(e))
        scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(e)));
      continue;
    }
    setEntityPose(scene, e, t.position, t.rotation);
  }
  time = snapshotTime_;
}

glm::mat4 World::entityMatrix(entt::entity e) const {
  if (!registry.valid(e) || !registry.all_of<Transform>(e)) return glm::mat4(1.0f);
  const Transform& t = registry.get<Transform>(e);
  if (const Shape* s = registry.try_get<Shape>(e)) return drawMatrix(t, *s);
  return glm::translate(glm::mat4(1.0f), t.position) * glm::mat4_cast(t.rotation);
}

void World::setEntityPose(scene::Scene& scene, entt::entity e, glm::vec3 position, glm::quat rotation) {
  if (!registry.valid(e) || !registry.all_of<Transform>(e)) return;
  Transform& t = registry.get<Transform>(e);
  t.position = position;
  t.rotation = glm::normalize(rotation);
  if (RigidBody* b = registry.try_get<RigidBody>(e); b && b->bodyId != ~0u) physics->setPose(b->bodyId, t.position, t.rotation);
  if (const Renderable* r = registry.try_get<Renderable>(e); r && registry.all_of<Shape>(e))
    scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(e)));
  // Modelos glTF e personagens: mover no editor fica para depois (o modelo tem vários objetos e colisão por malha).
}

bool World::playerFeet(glm::vec3& feet) const {
  for (auto ent : registry.view<Character>()) {
    const Character& c = registry.get<Character>(ent);
    if (c.player && c.id != ~0u) {
      feet = physics->characterFeet(c.id);
      return true;
    }
  }
  return false;
}

}  // namespace game
