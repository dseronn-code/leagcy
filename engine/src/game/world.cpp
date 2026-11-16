#include "game/world.h"
#include "core/carregamento.h"
#include "game/navigation.h"
#include "game/scripting.h"
#include "game/visual_script.h"
#include "game/node_material.h"
#include "util/png.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <cmath>
#include <fstream>
#include <memory>
#include <random>
#include <unordered_map>
#include <functional>
#include <cstring>
#include <map>

#include <stb_image.h>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "core/log.h"
#include "core/paths.h"
#include "scene/camera.h"
#include "scene/demo_scene.h"
#include "scene/mesh.h"
#include "scene/scene.h"

namespace game {

using json = nlohmann::json;

namespace {

// ---- Multidão (cena .json, "multidao") ----
// Pessoas: cada glTF com esqueleto recebe por retargeting o clipe nomeado (clipNames[k % n] para o k-ésimo) do arquivo
// de animações e é "assado" em poses, uma por peça (corpo, roupa, cabelo, olhos...). Devolve as peças de cada pessoa.
std::vector<std::vector<uint32_t>> bakeCrowdPeople(scene::Scene& scene, const std::function<std::string(std::string)>& path,
                                                   const json& people, const std::string& animFile,
                                                   const std::vector<std::string>& clipNames,
                                                   const std::vector<std::pair<std::string, std::string>>& map,
                                                   uint32_t poses) {
  std::vector<std::vector<uint32_t>> clips;
  int src = -1;
  if (!scene::loadGltf(scene, path(animFile), glm::mat4(1.0f), &src) || src < 0) {
    LOG_WARN("Multidão: não carreguei as animações %s", animFile.c_str());
    return clips;
  }
  scene.models[size_t(src)].animation = -1;
  size_t personIndex = 0;
  for (const json& pf : people) {
    const std::string clipName = clipNames[personIndex++ % clipNames.size()];
    int model = -1;
    const std::string file = path(pf.get<std::string>());
    if (!scene::loadGltf(scene, file, glm::mat4(1.0f), &model) || model < 0) {
      LOG_WARN("Multidão: não carreguei %s", file.c_str());
      continue;
    }
    scene::Model& m = scene.models[size_t(model)];
    int a = -1;
    for (size_t k = 0; k < scene.models[size_t(src)].animations.size(); ++k)
      if (scene.models[size_t(src)].animations[k].name == clipName)
        a = scene::retargetAnimation(m, scene.models[size_t(src)], int(k), map, clipName);
    if (a < 0) {
      LOG_WARN("Multidão: sem o clipe \"%s\" para %s", clipName.c_str(), file.c_str());
      continue;
    }
    m.animation = a;
    std::vector<uint32_t> parts;
    for (uint32_t k = 0; k < uint32_t(m.skinnedObjects.size()); ++k) parts.push_back(scene.bakeCrowdClip(model, poses, k));
    clips.push_back(parts);
    m.root = glm::translate(glm::mat4(1.0f), {0.0f, -1000.0f, 0.0f});  // o original fica escondido
  }
  return clips;
}

// Uma pessoa da multidão: as peças dela com a mesma fase do passo; roupa e cabelo com cor sorteada de paletas
// realistas (cada agente com a sua cópia do material), para os modelos não parecerem clones.
std::vector<uint32_t> spawnCrowdPerson(scene::Scene& scene, const std::vector<uint32_t>& person, const glm::mat4& transform,
                                       float phase, std::mt19937& rng) {
  std::uniform_real_distribution<float> u(0.0f, 1.0f);
  static const glm::vec3 clothes[] = {{0.25f, 0.27f, 0.35f}, {0.12f, 0.12f, 0.13f}, {0.75f, 0.68f, 0.55f},
                                      {0.62f, 0.15f, 0.12f}, {0.35f, 0.38f, 0.25f}, {0.55f, 0.55f, 0.57f},
                                      {1.0f, 1.0f, 1.0f},    {0.2f, 0.3f, 0.55f},   {0.45f, 0.28f, 0.18f},
                                      {0.9f, 0.85f, 0.75f},  {0.3f, 0.12f, 0.3f},   {1.0f, 0.75f, 0.2f}};
  static const glm::vec3 hairs[] = {{0.25f, 0.2f, 0.18f}, {0.5f, 0.38f, 0.28f}, {1.0f, 0.85f, 0.6f},
                                    {0.15f, 0.13f, 0.12f}, {0.75f, 0.75f, 0.75f}, {0.6f, 0.3f, 0.18f}};
  const glm::vec3 shirtTint = clothes[size_t(u(rng) * 12.0f) % 12], hairTint = hairs[size_t(u(rng) * 6.0f) % 6];
  const bool tintClothes = u(rng) < 0.75f;
  std::vector<uint32_t> agent;
  for (uint32_t clip : person) {
    const uint32_t o = scene.addCrowdAgent(clip, transform, phase);
    scene::Material m = scene.materials[scene.objects[o].material];
    const uint32_t f = m.flags;
    const bool skinOrEye = (f & (scene::kMaterialSubsurface | scene::kMaterialEye | scene::kMaterialClearcoat)) != 0;
    const bool hairLike = (f & scene::kMaterialAlphaMask) != 0;
    if (!skinOrEye && (hairLike || tintClothes)) {
      const glm::vec3 t = hairLike ? hairTint : glm::mix(glm::vec3(1.0f), shirtTint, 0.85f);
      m.baseColor = glm::vec4(glm::vec3(m.baseColor) * t, m.baseColor.a);
      scene.objects[o].material = scene.addMaterial(m);
    }
    agent.push_back(o);
  }
  return agent;
}

}  // namespace

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
std::string gLanguageOverride;  // --idioma

// Velocidade inicial lida do arquivo (aplicada ao criar o corpo).
struct InitialVelocity {
  glm::vec3 value;
};

}  // namespace

// O que o arquivo trazia além das entidades: volta igual no save().
struct World::FileData {
  json sun, camera, materials, extra;
};

namespace {

// Põe o modelo de um personagem nos pés dele, virado para `v.yaw` (radianos; 0 = +Z).
void placeVisual(scene::Model& m, const CharacterVisual& v, glm::vec3 feet) {
  const glm::quat facing = glm::angleAxis(v.yaw, glm::vec3(0.0f, 1.0f, 0.0f));
  m.root = glm::translate(glm::mat4(1.0f), feet + facing * v.offset) *
           glm::mat4_cast(facing * glm::angleAxis(glm::radians(v.yawDeg), glm::vec3(0.0f, 1.0f, 0.0f))) *
           glm::scale(glm::mat4(1.0f), glm::vec3(v.scale));
  m.groundBase = feet.y;
  if (m.layers.empty() && v.idleA >= 0) m.layers.push_back({v.idleA, 0.0f, 1.0f});
}

// Pesos de uma mistura 1D por velocidade: pontos (0, parado), (walkRef, andar), (runRef, correr), só os que existem.
void locomotionWeights(const CharacterVisual& v, float speed, float& wIdle, float& wWalk, float& wRun) {
  struct P { float s; float* w; };
  std::vector<P> pts;
  wIdle = wWalk = wRun = 0.0f;
  if (v.idleA >= 0) pts.push_back({0.0f, &wIdle});
  if (v.walkA >= 0) pts.push_back({v.walkRef, &wWalk});
  if (v.runA >= 0) pts.push_back({v.runRef, &wRun});
  if (pts.empty()) return;
  if (speed <= pts.front().s) { *pts.front().w = 1.0f; return; }
  if (speed >= pts.back().s) { *pts.back().w = 1.0f; return; }
  for (size_t i = 0; i + 1 < pts.size(); ++i)
    if (speed <= pts[i + 1].s) {
      const float f = (speed - pts[i].s) / std::max(pts[i + 1].s - pts[i].s, 1e-4f);
      *pts[i].w = 1.0f - f;
      *pts[i + 1].w = f;
      return;
    }
}

}  // namespace

World::World() : physics(std::make_unique<Physics>()), file_(std::make_unique<FileData>()) {}
World::~World() {
  scripting.reset();  // o Lua guarda referências às entidades
  navigation.reset();
  audio.reset();
  registry.clear();   // antes da física (os corpos são destruídos com ela)
  if (gCurrent == this) gCurrent = nullptr;
}

bool World::load(const std::string& path, scene::Scene& scene, scene::Camera& camera) {
  this->scene = &scene;
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
  scenePath = path;
  if (doc.contains("mundo_aberto") && doc["mundo_aberto"].is_object()) {
    const json& ow = doc["mundo_aberto"];
    openWorld.enabled = true;
    openWorld.cellSize = std::max(4.0f, ow.value("celula", openWorld.cellSize));
    openWorld.loadDistance = ow.value("carregar", openWorld.loadDistance);
    openWorld.unloadDistance = std::max(openWorld.loadDistance + 1.0f, ow.value("descarregar", openWorld.loadDistance * 1.3f));
  }
  // Geometria virtualizada (tipo Nanite) para a cena inteira: "nanite": true (o --nanite / --no-nanite vale acima).
  if (doc.contains("nanite") && doc["nanite"].is_boolean()) scene.clusterLod = doc["nanite"].get<bool>();
  // Clima localizado: "tempestades": [{"centro": [x, z], "velocidade": [vx, vz], "raio": 250, "intensidade": 1}, ...]
  // (até 4). A chuva da cena ("chuva") só cai dentro das células, que andam; o chão continua molhado depois.
  scene.storms.clear();
  if (doc.contains("tempestades") && doc["tempestades"].is_array())
    for (const json& t : doc["tempestades"]) {
      if (!t.is_object() || scene.storms.size() >= 4) continue;
      scene::Scene::Storm st;
      if (t.contains("centro") && t["centro"].is_array() && t["centro"].size() >= 2)
        st.center = {t["centro"][0].get<float>(), t["centro"][1].get<float>()};
      if (t.contains("velocidade") && t["velocidade"].is_array() && t["velocidade"].size() >= 2)
        st.velocity = {t["velocidade"][0].get<float>(), t["velocidade"][1].get<float>()};
      st.radius = t.value("raio", 250.0f);
      st.intensity = t.value("intensidade", 1.0f);
      scene.storms.push_back(st);
    }
  loadLanguages();
  {
    std::string saved;
    std::ifstream("saves/idioma.txt") >> saved;
    language = "pt";
    const std::string fromScene = doc.value("idioma", std::string());
    for (const std::string& want : {fromScene, saved, gLanguageOverride})
      if (!want.empty() && texts_.count(want)) language = want;
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
        json graphFile;
        if (m.contains("grafo") && m["grafo"].is_string()) {  // o grafo num arquivo (o editor de grafos grava nele)
          std::string gp = m["grafo"].get<std::string>();
          const size_t slash = path.find_last_of("/\\");
          const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
          if (!std::ifstream(gp) && std::ifstream(dir + gp)) gp = dir + gp;
          std::ifstream gf(gp);
          try {
            graphFile = json::parse(gf);
          } catch (const json::exception& ex) {
            LOG_ERROR("Material %s: grafo %s inválido (%s)", name.c_str(), gp.c_str(), ex.what());
          }
        }
        const json& mg = graphFile.is_object() ? graphFile : m;
        if (mg.contains("nos")) {  // material por nós: assado em texturas (node_material.h)
          const json& m = mg;
          NodeMaterialBake bake;
          std::string error;
          const uint32_t res = std::clamp(m.value("resolucao", 512u), 16u, 4096u);
          if (bakeNodeMaterial(m, res, bake, error)) {
            if (const char* dump = std::getenv("LEGACY_DUMP_MATERIAIS")) {  // testes: as texturas assadas em PNG
              const std::string base = std::string(dump) + "/" + name;
              util::writePng(base + "_cor.png", bake.color.rgba.data(), res, res);
              util::writePng(base + "_rugmetal.png", bake.metalRough.rgba.data(), res, res);
              if (bake.hasNormal) util::writePng(base + "_normal.png", bake.normal.rgba.data(), res, res);
            }
            mat.baseColor = glm::vec4(1.0f);  // as texturas já têm os valores (os fatores do glTF multiplicam)
            mat.roughness = 1.0f;
            mat.metallic = 1.0f;
            mat.baseColorTex = int32_t(scene.addTexture(std::move(bake.color)));
            mat.metalRoughTex = int32_t(scene.addTexture(std::move(bake.metalRough)));
            if (bake.hasNormal) mat.normalTex = int32_t(scene.addTexture(std::move(bake.normal)));
            LOG_INFO("Material %s: grafo de %zu nós assado em %ux%u", name.c_str(), m["nos"].size(), res, res);
          } else {
            LOG_ERROR("Material %s: %s", name.c_str(), error.c_str());
          }
        }
        if (m.value("terreno", false)) {  // grama, terra, rocha e cascalho pela inclinação e por ruído (material.glsl)
          mat.flags |= scene::kMaterialTerrain;
          if (scene.terrainLayers.albedo[0] < 0) scene::addTerrainLayers(scene);
        }
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
    // Multijogador: o jogador da cena vira o modelo das vagas remotas (cópias estacionadas fora do mapa).
    json entities = doc.value("entidades", json::array());
    if (doc.contains("multijogador")) {
      const json& mj = doc["multijogador"];
      const int slots = std::clamp(mj.value("vagas", 3), 0, 16);
      const json* tmpl = nullptr;
      for (const json& e : entities)
        if (e.contains("personagem") && e["personagem"].value("controle", "jogador") == "jogador" &&
            !e["personagem"].contains("roteiro"))
          tmpl = &e;
      if (tmpl) {
        const glm::vec3 base = vec3Of(tmpl->value("posicao", json()), glm::vec3(0.0f));
        const json spawns = mj.value("nascer", json::array());
        json extra = json::array();
        for (int i = 0; i < slots; ++i) {
          json c = *tmpl;
          c["nome"] = tmpl->value("nome", std::string("jogador")) + "_" + std::to_string(i + 2);
          c["personagem"]["controle"] = "remoto";
          c["personagem"]["vaga"] = i;
          c["posicao"] = json::array({base.x, -1000.0f - 10.0f * float(i), base.z});
          extra.push_back(c);
          slotSpawn.push_back(i < int(spawns.size()) ? vec3Of(spawns[size_t(i)], base)
                                                     : base + glm::vec3(1.5f * float(i + 1), 0.0f, 0.0f));
        }
        for (auto& c : extra) entities.push_back(c);
        remoteInputs.assign(size_t(slots), PlayerInput{});
        slotActive.assign(size_t(slots), false);
      } else {
        LOG_WARN("Cena: \"multijogador\" sem personagem com \"controle\": \"jogador\" para copiar");
      }
    }
    for (const json& e : entities) {
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
            for (const json& pt : ia.value("patrulha", json::array())) c.aiPatrol.push_back(vec3Of(pt, glm::vec3(0.0f)));
            c.aiChase = ia.value("perseguir", "");
            c.aiFlee = ia.value("fugir_de", "");
            c.aiSight = ia.value("visao", c.aiSight);
            c.aiFovDeg = ia.value("campo_visao", c.aiFovDeg);
            if (!c.aiPatrol.empty() || !c.aiChase.empty() || !c.aiFlee.empty()) c.aiHasTarget = true;
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
        if (pj.value("controle", "") == "remoto") {
          c.player = false;
          c.remoteSlot = pj.value("vaga", 0);
        }
        if (pj.contains("visual")) {  // modelo glTF animado no lugar da cápsula
          const json& vj = pj["visual"];
          CharacterVisual& v = c.visual;
          v.enabled = true;
          v.path = vj.value("modelo", "");
          if (!v.path.empty() && !std::ifstream(v.path) && std::ifstream(sceneDir + v.path)) v.path = sceneDir + v.path;
          v.scale = vj.value("escala", v.scale);
          v.offset = vec3Of(vj.value("desvio", json()), v.offset);
          v.autoOffset = !vj.contains("desvio");
          v.yawDeg = vj.value("girar_graus", v.yawDeg);
          v.idle = vj.value("parado", "");
          v.walk = vj.value("andar", "");
          v.run = vj.value("correr", "");
          v.walkRef = vj.value("velocidade_andar", v.walkRef);
          v.runRef = vj.value("velocidade_correr", v.runRef);
          v.fade = vj.value("transicao", v.fade);
          v.ik = vj.value("ik_pes", v.ik);
          for (const json& leg : vj.value("pernas", json::array()))
            if (leg.is_array() && leg.size() == 3)
              v.legs.push_back({leg[0].get<std::string>(), leg[1].get<std::string>(), leg[2].get<std::string>()});
          if (vj.contains("retarget")) {  // animações de outro esqueleto
            const json& rj = vj["retarget"];
            v.retargetFrom = rj.value("de", "");
            if (!v.retargetFrom.empty() && !std::ifstream(v.retargetFrom) && std::ifstream(sceneDir + v.retargetFrom))
              v.retargetFrom = sceneDir + v.retargetFrom;
            v.retargetSrcPrefix = rj.value("prefixo_origem", "");
            v.retargetDstPrefix = rj.value("prefixo_destino", "");
            const json mapa = rj.value("mapa", json::object());  // cópia: items() de um temporário apontaria para nada
            for (auto& [a, b] : mapa.items()) v.retargetMap.push_back({a, b.get<std::string>()});
          }
          if (vj.contains("cabelo_fios")) {  // fios simulados numa calota presa ao osso da cabeça
            const json& hj = vj["cabelo_fios"];
            v.hairBone = hj.value("osso", "head");
            v.hairStrands = std::clamp(hj.value("fios", 16000u), 100u, 200000u);
            v.hairSegments = std::clamp(hj.value("segmentos", 12u), 2u, 32u);
            v.hairSeed = hj.value("semente", 3u);
            v.hairLength = hj.value("comprimento", v.hairLength);
            v.hairHalfWidth = hj.value("espessura", v.hairHalfWidth);
            v.hairCenter = vec3Of(hj.value("centro", json()), v.hairCenter);
            v.hairRadius = vec3Of(hj.value("raio", json()), v.hairRadius);
            v.hairColor = vec3Of(hj.value("cor", json()), v.hairColor);
          }
        }
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
      std::unique_ptr<ParticleSource> particles;
      if (e.contains("particulas")) {
        const json& pj = e["particulas"];
        particles = std::make_unique<ParticleSource>();
        particles->type = pj.value("tipo", "fogo");
        particles->radius = pj.value("raio", particles->radius);
        particles->scale = pj.value("escala", particles->scale);
        particles->count = pj.value("quantidade", particles->type == "fogo" ? 900u : particles->type == "fumaca" ? 500u : 150u);
        particles->offset = vec3Of(pj.value("desvio", json()), particles->offset);
        particles->enabled = pj.value("ligado", true);
        if (particles->type != "fogo" && particles->type != "fumaca" && particles->type != "faiscas") {
          LOG_WARN("Cena: partículas '%s' desconhecidas (fogo, fumaca, faiscas)", particles->type.c_str());
          particles.reset();
        }
      }
      std::string script = e.value("script", "");
      if (!script.empty() && !std::ifstream(script) && std::ifstream(sceneDir + script)) script = sceneDir + script;
      if (script.empty() && e.contains("script_visual")) script = compileVisualSource(e["script_visual"], sceneDir);

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
            if (e.value("sempre_carregado", false)) registry.emplace<AlwaysLoaded>(ent);
            if (e.value("no_chao", false)) registry.emplace<OnGround>(ent);
            if (character && character->visual.enabled) {
              Character ch = *character;
              CharacterVisual& v = ch.visual;
              int model = -1;
              if (scene::loadGltf(scene, v.path, glm::mat4(1.0f), &model) && model >= 0) {
                v.model = model;
                if (!v.retargetFrom.empty()) retargetCharacter(scene, model, v);
                if (v.hairStrands > 0) {
                  v.hairNode = scene.models[size_t(model)].findNode(v.hairBone);
                  if (v.hairNode < 0) {
                    LOG_WARN("Personagem %s: osso \"%s\" do cabelo não existe", name.c_str(), v.hairBone.c_str());
                  } else {
                    scene::Material hm;
                    hm.baseColor = glm::vec4(v.hairColor, 1.0f);
                    hm.roughness = 0.35f;
                    hm.flags = scene::kMaterialHair | scene::kMaterialDoubleSided;
                    v.hairObject = int(scene.addObject(
                        scene.addMesh(scene::makeHair(v.hairStrands, v.hairSegments, 1.0f, v.hairLength, v.hairHalfWidth, v.hairSeed)),
                        glm::mat4(1.0f), scene.addMaterial(hm)));
                  }
                }
                scene::Model& sm = scene.models[size_t(model)];
                v.idleA = sm.findAnimation(v.idle);
                v.walkA = sm.findAnimation(v.walk);
                v.runA = sm.findAnimation(v.run);
                for (const auto& leg : v.legs) {
                  const int a = sm.findNode(leg[0]), b = sm.findNode(leg[1]), f = sm.findNode(leg[2]);
                  if (a < 0 || b < 0 || f < 0) {
                    LOG_WARN("Personagem %s: perna %s/%s/%s não existe no modelo", name.c_str(), leg[0].c_str(),
                             leg[1].c_str(), leg[2].c_str());
                    continue;
                  }
                  sm.legs.push_back({uint32_t(a), uint32_t(b), uint32_t(f)});
                }
                const glm::vec3 feet = t.position - glm::vec3(0.0f, 0.5f * ch.desc.height, 0.0f);
                placeVisual(sm, v, feet);
                if (v.autoOffset) {  // sem "desvio": o ponto mais baixo da pose parada fica no pé da cápsula
                  scene.poseModel(model);
                  glm::vec3 lo, hi;
                  scene.modelBounds(model, lo, hi);
                  v.offset.y = feet.y - lo.y;
                  v.autoOffset = false;
                  placeVisual(sm, v, feet);
                }
                scene.poseModel(model);
                if (v.hairObject >= 0 && v.hairNode >= 0 && size_t(v.hairNode) < sm.nodeWorld.size()) {
                  // Já na cabeça desde o início: criado na origem, o salto no primeiro quadro viraria um tranco enorme
                  // na simulação dos fios.
                  scene.setTransform(uint32_t(v.hairObject), sm.nodeWorld[size_t(v.hairNode)] *
                                                                 glm::translate(glm::mat4(1.0f), v.hairCenter) *
                                                                 glm::scale(glm::mat4(1.0f), v.hairRadius));
                }
                registry.emplace<Character>(ent, ch);
              } else {
                LOG_WARN("Personagem %s: não carreguei o modelo %s; fica a cápsula", name.c_str(), v.path.c_str());
                ch.visual.enabled = false;
                registry.emplace<Renderable>(ent, scene.addObject(mesh, drawMatrix(t, shape), mit->second), matName);
                registry.emplace<Character>(ent, ch);
              }
            } else {
              registry.emplace<Renderable>(ent, scene.addObject(mesh, drawMatrix(t, shape), mit->second), matName);
              if (character) registry.emplace<Character>(ent, *character);
            }
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
            if (particles) {
              ParticleSource ps = *particles;
              const scene::ParticleType type = ps.type == "fogo" ? scene::kParticleFire
                                               : ps.type == "fumaca" ? scene::kParticleSmoke : scene::kParticleSparks;
              ps.emitter = scene.addEmitter(type, t.position + ps.offset, ps.radius, ps.scale, ps.count);
              if (!ps.enabled) scene.setEmitter(ps.emitter, t.position + ps.offset, 0.0f);
              registry.emplace<ParticleSource>(ent, ps);
            }
            if (hasBody && !character && !vehicle) {
              registry.emplace<RigidBody>(ent, body);
              if (glm::dot(velocity, velocity) > 0.0f) registry.emplace<InitialVelocity>(ent, velocity);
            }
            ++created;
          }
    }
    // Multidão: várias pessoas (glTF com esqueleto, ex.: tools/humano.py) com a animação de andar copiada de outro
    // esqueleto (retargeting) e "assada" em poses (sem skinning por pessoa); os agentes andam nos dois sentidos por
    // faixas e desviam de obstáculos e uns dos outros. "parados": gente em pé (no celular, olhando os telões...)
    // espalhada em áreas retangulares.
    //   "multidao": {"pessoas": [...], "animacoes": "anim.gltf", "clipes": ["andar", ...], "mapa": {...},
    //                "poses": 16, "quantidade": 600, "altura": 0.15, "girar_graus": 0, "semente": 1,
    //                "velocidade": [1.1, 1.6], "faixas": [{"de": [x, z], "ate": [x, z], "largura": 4}, ...],
    //                "obstaculos": [[x, z, raio], ...],
    //                "parados": {"pessoas": [...], "animacoes": "...", "clipes": [...], "quantidade": 200,
    //                            "areas": [[x0, z0, x1, z1], ...], "olhar_para": [x, z]}}
    if (doc.contains("multidao") && doc["multidao"].is_object()) {
      const json& mj = doc["multidao"];
      const std::function<std::string(std::string)> path = [&](std::string f) {
        if (!f.empty() && !std::ifstream(f) && std::ifstream(sceneDir + f)) f = sceneDir + f;
        return f;
      };
      std::vector<std::pair<std::string, std::string>> map;
      const json mapJson = mj.value("mapa", json::object());  // cópia: items() de um temporário fica pendurado
      for (auto& [a, b] : mapJson.items()) map.push_back({a, b.get<std::string>()});
      auto names = [](const json& j, const char* fallback) {
        std::vector<std::string> out;
        const json list = j.value("clipes", json::array());
        for (const json& c : list) out.push_back(c.get<std::string>());
        if (out.empty()) out.push_back(j.value("clipe", std::string(fallback)));
        return out;
      };
      const uint32_t poses = std::clamp(mj.value("poses", 24), 4, 128);
      std::mt19937 rng(mj.value("semente", 1u));
      std::uniform_real_distribution<float> u(0.0f, 1.0f);
      const float y = mj.value("altura", 0.0f), yaw0 = glm::radians(mj.value("girar_graus", 0.0f));
      const auto clips = bakeCrowdPeople(scene, path, mj.value("pessoas", json::array()), mj.value("animacoes", ""),
                                         names(mj, "andar"), map, poses);
      struct Lane { glm::vec2 a, b; float width, length; };
      std::vector<Lane> lanes;
      float total = 0.0f;
      const json lanesJson = mj.value("faixas", json::array());
      for (const json& fj : lanesJson) {
        Lane l{{fj["de"][0].get<float>(), fj["de"][1].get<float>()}, {fj["ate"][0].get<float>(), fj["ate"][1].get<float>()},
               fj.value("largura", 3.0f), 0.0f};
        l.length = glm::length(l.b - l.a);
        if (l.length > 1.0f) {
          lanes.push_back(l);
          total += l.length;
        }
      }
      // Obstáculos ([x, z, raio], ex.: bancas, bancos, entradas do metrô): a pessoa contorna pela lateral.
      std::vector<glm::vec3> obstacles;
      // Lista na cena ou o nome de um .json com ela (a da cidade: tools/times_square.py grava obstaculos.json com tudo
      // o que fica na calçada — árvores, lixeiras, carrinhos, bancas — e a multidão contorna cada um).
      json obsJson = mj.value("obstaculos", json::array());
      if (obsJson.is_string()) {
        std::ifstream of(path(obsJson.get<std::string>()));
        obsJson = of ? json::parse(of, nullptr, false) : json::array();
        if (!obsJson.is_array()) obsJson = json::array();
      }
      for (const json& o : obsJson)
        if (o.is_array() && o.size() >= 3) obstacles.push_back({o[0].get<float>(), o[1].get<float>(), o[2].get<float>()});
      if (!clips.empty() && !lanes.empty()) {
        const json vel = mj.value("velocidade", json::array({1.1, 1.6}));
        const float vmin = vel[0].get<float>(), vmax = vel[1].get<float>();
        const int count = std::max(0, mj.value("quantidade", 300));
        // Estado de todos os agentes num só animador: cada um anda pela faixa e ajusta o deslocamento lateral para
        // não atravessar quem está logo à frente (grade de 1,5 m para achar os vizinhos).
        struct Agent {
          Lane L;
          float dir, baseSide, side, speed, s0, scale;
          std::vector<uint32_t> objs;
          glm::vec2 pos{0.0f}, prevPos{0.0f};
          float yaw = 0.0f;  // direção do corpo, suavizada (vira para onde anda, sem estalo)
          bool started = false;
          // Temperamento: o ritmo varia devagar em volta da velocidade própria (amplitude e período por pessoa); quem
          // não consegue passar por alguém logo à frente diminui o passo atrás dele.
          float s = 0.0f, cur = 0.0f, ritmoAmp = 0.05f, ritmoW = 0.3f, ritmoFase = 0.0f;
          // Reação a um esbarrão (personagem a mais de 1,2 m/s encostando): a pessoa é empurrada na direção do
          // golpe, freia, gira o corpo para fora e inclina para o lado do empurrão, voltando ao passo em ~1 s.
          float tropeco = 0.0f, giro = 0.0f;
          glm::vec2 empurrao{0.0f}, eixoQueda{0.0f};
        };
        // Obstáculos numa grade de 4 m (2.000 pessoas × 1.100 obstáculos por quadro seria lento).
        auto obsGrid = std::make_shared<std::unordered_map<int64_t, std::vector<glm::vec3>>>();
        auto okey = [](int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
        for (const glm::vec3& o : obstacles) {
          const int r = int(std::ceil(o.z / 4.0f));
          const int cx = int(std::floor(o.x / 4.0f)), cz = int(std::floor(o.y / 4.0f));
          for (int gx = -r; gx <= r; ++gx)
            for (int gz = -r; gz <= r; ++gz) (*obsGrid)[okey(cx + gx, cz + gz)].push_back(o);
        }
        auto agents = std::make_shared<std::vector<Agent>>();
        agents->reserve(size_t(count));
        for (int i = 0; i < count; ++i) {
          float pick = u(rng) * total;  // faixa sorteada pelo comprimento (gente espalhada por igual)
          size_t li = 0;
          while (li + 1 < lanes.size() && pick > lanes[li].length) pick -= lanes[li++].length;
          const Lane L = lanes[li];
          const float dir = u(rng) < 0.5f ? 1.0f : -1.0f, side = (u(rng) - 0.5f) * L.width;
          const float speed = vmin + (vmax - vmin) * u(rng), s0 = u(rng) * L.length, scale = 0.94f + 0.12f * u(rng);
          const std::vector<uint32_t>& person = clips[size_t(u(rng) * float(clips.size())) % clips.size()];
          const float phase = 4.0f * u(rng);
          auto objs = spawnCrowdPerson(scene, person, glm::mat4(1.0f), phase, rng);
          // O ciclo de passada das capturas (100STYLE) corresponde a ~1,35 m/s: quem anda mais rápido dá o passo mais
          // rápido (o pé fica plantado no chão em vez de patinar).
          for (uint32_t o : objs) scene.objects[o].animRate = glm::clamp(speed / 1.35f, 0.8f, 1.25f);
          Agent ag{L, dir, side, side, speed, s0, scale, std::move(objs)};
          ag.s = s0;
          ag.cur = speed;
          const bool apressado = speed > vmin + 0.7f * (vmax - vmin);
          ag.ritmoAmp = apressado ? 0.03f + 0.03f * u(rng) : 0.05f + 0.1f * u(rng);  // quem passeia varia mais
          ag.ritmoW = 0.15f + 0.35f * u(rng);
          ag.ritmoFase = 6.2831853f * u(rng);
          agents->push_back(std::move(ag));
        }
        scene.addAnimator([this, agents, obsGrid, okey, y, yaw0](scene::Scene& sc, float t, float dt) {
          std::vector<Agent>& A = *agents;
          const float step = std::min(dt, 0.1f);
          constexpr float kTropeco = 0.9f;
          // Personagens da cena (o jogador e os de roteiro/IA): posição e velocidade no chão. A multidão abre espaço
          // para eles e reage quando é atropelada.
          std::vector<std::pair<glm::vec2, glm::vec2>> pers;
          if (physics)
            for (auto ent : registry.view<Character>()) {
              const Character& c = registry.get<Character>(ent);
              if (c.id == ~0u || c.ragdoll.active || c.remoteSlot >= 0) continue;
              const glm::vec3 p = physics->characterFeet(c.id), v = physics->characterVelocity(c.id);
              if (std::abs(p.y - y) < 2.5f) pers.push_back({{p.x, p.z}, {v.x, v.z}});
            }
          // 1) Posição ao longo da faixa e desvio dos obstáculos olhando até 4 m à frente: a pessoa escolhe o lado
          //    e muda o caminho aos poucos (~0,9 m/s de lado), em vez de ser jogada para a borda ao encostar.
          for (Agent& a : A) {
            const glm::vec2 d = (a.L.b - a.L.a) / a.L.length, n(-d.y, d.x), f = d * a.dir;
            if (a.tropeco > 0.0f) {  // o empurrão desloca a pessoa (ao longo e de lado) e some em ~0,3 s
              a.s += glm::dot(a.empurrao, f) * step;
              a.side = glm::clamp(a.side + glm::dot(a.empurrao, n) * step, -0.5f * a.L.width - 0.2f,
                                  0.5f * a.L.width + 0.2f);
              a.empurrao *= std::exp(-6.0f * step);
              a.tropeco = std::max(0.0f, a.tropeco - step);
            }
            a.s = std::fmod(a.s + a.cur * step + a.L.length, a.L.length);
            const float s = a.s;
            const glm::vec2 c = a.dir > 0.0f ? a.L.a + d * s : a.L.b - d * s;  // ponto no eixo da faixa
            float alvo = a.side;
            bool desviar = false;
            const glm::vec2 frente = c + n * a.side + f * 2.0f;
            const int cx = int(std::floor(frente.x / 4.0f)), cz = int(std::floor(frente.y / 4.0f));
            for (int gx = -1; gx <= 1; ++gx)
              for (int gz = -1; gz <= 1; ++gz) {
                const auto it = obsGrid->find(okey(cx + gx, cz + gz));
                if (it == obsGrid->end()) continue;
                for (const glm::vec3& o : it->second) {
                  const glm::vec2 r = glm::vec2(o.x, o.y) - c;
                  const float along = glm::dot(r, f), across = glm::dot(r, n), folga = o.z + 0.35f;
                  if (along < -0.6f || along > 4.0f || std::abs(across - a.side) > folga) continue;
                  // passa pelo lado mais perto do caminho atual
                  const float esq = across + folga, dir = across - folga;
                  const float novo = std::abs(esq - a.side) < std::abs(dir - a.side) ? esq : dir;
                  if (!desviar || std::abs(novo - a.side) > std::abs(alvo - a.side)) alvo = novo;
                  desviar = true;
                }
              }
            if (desviar) a.side += glm::clamp(alvo - a.side, -0.9f * step, 0.9f * step);
            a.prevPos = a.started ? a.pos : c + n * a.side;
            a.pos = c + n * a.side;
          }
          // 2) Desvio: quem tem alguém a menos de 0,8 m à frente puxa o lado para longe dele (~1 m/s de lado); sem
          //    ninguém, volta aos poucos para o próprio caminho.
          std::unordered_map<int64_t, std::vector<uint32_t>> grid;
          auto key = [](int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); };
          for (uint32_t i = 0; i < A.size(); ++i)
            grid[key(int(std::floor(A[i].pos.x / 1.5f)), int(std::floor(A[i].pos.y / 1.5f)))].push_back(i);
          for (uint32_t i = 0; i < A.size(); ++i) {
            Agent& a = A[i];
            const glm::vec2 d = (a.L.b - a.L.a) / a.L.length, n(-d.y, d.x), f = d * a.dir;
            float push = 0.0f, frenteVel = 1e9f;
            const int cx = int(std::floor(a.pos.x / 1.5f)), cz = int(std::floor(a.pos.y / 1.5f));
            for (int gx = -1; gx <= 1; ++gx)
              for (int gz = -1; gz <= 1; ++gz) {
                const auto it = grid.find(key(cx + gx, cz + gz));
                if (it == grid.end()) continue;
                for (uint32_t j : it->second) {
                  if (j == i) continue;
                  const glm::vec2 r = A[j].pos - a.pos;
                  const float dist = glm::length(r);
                  if (dist < 0.8f && glm::dot(r, f) > -0.1f) {
                    const float across = glm::dot(r, n);
                    push += (across > 0.0f || (across == 0.0f && i < j) ? -1.0f : 1.0f) * (0.8f - dist);
                  }
                  // alguém logo à frente, no mesmo sentido e quase na mesma linha: acompanha o passo dele
                  if (dist < 1.4f && glm::dot(r, f) > 0.3f && std::abs(glm::dot(r, n)) < 0.45f && A[j].dir == a.dir)
                    frenteVel = std::min(frenteVel, A[j].cur);
                }
              }
            // Desvio só dentro da faixa (+20 cm): com +60 cm, quem desviava encostava na fachada e aparecia "dentro" das
            // vitrines das lojas.
            // Personagens: espaço pessoal maior (1,6 m) que entre desconhecidos da multidão; quem está parado bem na
            // frente faz a pessoa frear e contornar. Encostar correndo derruba o passo dela (esbarrão).
            for (const auto& [pp, pv] : pers) {
              const glm::vec2 r = pp - a.pos;
              const float dist = glm::length(r), along = glm::dot(r, f), across = glm::dot(r, n);
              if (dist < 1.6f && along > -0.3f) push += (across >= 0.0f ? -1.0f : 1.0f) * (1.6f - dist) * 0.6f;
              if (dist < 1.5f && along > 0.2f && std::abs(across) < 0.5f) frenteVel = std::min(frenteVel, 0.5f);
              const float vel = glm::length(pv);
              if (dist < 0.6f && vel > 1.2f && a.tropeco <= 0.0f) {
                const glm::vec2 golpe = pv / vel, fora = dist > 1e-3f ? -r / dist : golpe;
                a.empurrao = glm::normalize(golpe + fora) * std::min(vel, 5.0f) * 0.7f;
                a.eixoQueda = glm::normalize(a.empurrao);
                a.giro = (glm::dot(golpe, n) * a.dir >= 0.0f ? 1.0f : -1.0f) * 0.7f;
                a.tropeco = kTropeco;
                a.cur *= 0.3f;
              }
            }
            float alvoVel = a.speed * (1.0f + a.ritmoAmp * std::sin(a.ritmoW * t + a.ritmoFase));
            if (frenteVel < alvoVel) alvoVel = std::max(0.4f, frenteVel * 0.95f);
            if (a.tropeco > 0.0f) alvoVel = 0.25f;
            a.cur += (alvoVel - a.cur) * std::min(1.0f, 1.5f * step);
            for (uint32_t o : a.objs) sc.objects[o].animRate = glm::clamp(a.cur / 1.35f, 0.6f, 1.3f);
            const float half = a.L.width * 0.5f + 0.2f;
            if (push != 0.0f) a.side = glm::clamp(a.side + glm::clamp(push * 4.0f, -1.0f, 1.0f) * step, -half, half);
            else a.side += (a.baseSide - a.side) * std::min(1.0f, 0.3f * step);
          }
          // 3) Pose de cada pessoa: o corpo vira para onde ela de fato anda (desvio incluído), suavizado (~0,15 s).
          for (Agent& a : A) {
            const glm::vec2 d = (a.L.b - a.L.a) / a.L.length, f = d * a.dir;
            const glm::vec2 v = a.pos - a.prevPos;
            float alvoYaw = (a.started && glm::length(v) > 0.3f * step && a.tropeco <= 0.0f) ? std::atan2(v.x, v.y)
                                                                                               : std::atan2(f.x, f.y);
            const float k = a.tropeco / kTropeco;  // 1 no esbarrão, 0 recuperado
            alvoYaw += a.giro * k;
            if (!a.started) a.yaw = alvoYaw;
            float dy = std::remainder(alvoYaw - a.yaw, 6.2831853f);
            a.yaw += dy * std::min(1.0f, 7.0f * step);
            a.started = true;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), {a.pos.x, y, a.pos.y});
            if (k > 0.0f) {  // inclina pelos pés para o lado do empurrão e volta (sobe rápido, desce devagar)
              const float lean = 0.16f * std::sin(3.14159265f * std::pow(1.0f - k, 0.6f));
              m = glm::rotate(m, lean, glm::vec3(a.eixoQueda.y, 0.0f, -a.eixoQueda.x));
            }
            m = glm::rotate(m, a.yaw + yaw0, glm::vec3(0, 1, 0));
            for (uint32_t o : a.objs) sc.setTransform(o, glm::scale(m, glm::vec3(a.scale)));
          }
        });
        LOG_INFO("Multidão: %d pessoas andando (%zu modelos, %u poses cada) em %zu faixas", count, clips.size(), poses,
                 lanes.size());
      }
      // Gente parada: posições sorteadas nas áreas (longe dos obstáculos), olhando para um ponto com variação.
      if (mj.contains("parados") && mj["parados"].is_object()) {
        const json& pj = mj["parados"];
        const auto idle = bakeCrowdPeople(scene, path, pj.value("pessoas", json::array()), pj.value("animacoes", ""),
                                          names(pj, "parado"), map, poses);
        std::vector<glm::vec4> areas;
        const json areasJson = pj.value("areas", json::array());
        for (const json& a : areasJson)
          areas.push_back({a[0].get<float>(), a[1].get<float>(), a[2].get<float>(), a[3].get<float>()});
        const json look = pj.value("olhar_para", json());
        int made = 0;
        const int want = std::max(0, pj.value("quantidade", 100));
        for (int i = 0; i < want * 4 && made < want && !idle.empty() && !areas.empty(); ++i) {
          const glm::vec4 a = areas[size_t(u(rng) * float(areas.size())) % areas.size()];
          const glm::vec2 p(a.x + (a.z - a.x) * u(rng), a.y + (a.w - a.y) * u(rng));
          bool blocked = false;
          for (const glm::vec3& o : obstacles) blocked |= glm::length(p - glm::vec2(o.x, o.y)) < o.z;
          if (blocked) continue;
          float yaw = glm::two_pi<float>() * u(rng);
          if (look.is_array() && u(rng) < 0.7f) {  // a maioria olha para os telões, com ±40° de variação
            const glm::vec2 to = glm::vec2(look[0].get<float>(), look[1].get<float>()) - p;
            yaw = std::atan2(to.x, to.y) + (u(rng) - 0.5f) * 1.4f;
          }
          glm::mat4 m = glm::translate(glm::mat4(1.0f), {p.x, y, p.y});
          m = glm::rotate(m, yaw + yaw0, glm::vec3(0, 1, 0));
          m = glm::scale(m, glm::vec3(0.94f + 0.12f * u(rng)));
          spawnCrowdPerson(scene, idle[size_t(u(rng) * float(idle.size())) % idle.size()], m, 8.0f * u(rng), rng);
          ++made;
        }
        LOG_INFO("Multidão: %d pessoas paradas (%zu modelos)", made, idle.size());
      }
    }
    // Trânsito: carros (glTF parados, ex.: tools/carros.py) andando por faixas de rua num sentido, cada um mantendo
    // distância do da frente na mesma faixa; a pintura (material com verniz e sem emissão) ganha cor sorteada, menos
    // nos modelos com "taxi" no nome.
    //   "transito": {"carros": ["a.gltf", ...], "quantidade": 120, "semente": 1, "velocidade": [5, 11],
    //                "faixas": [{"de": [x, z], "ate": [x, z]}, ...], "altura": 0.0}
    if (doc.contains("transito") && doc["transito"].is_object()) {
      const json& tj = doc["transito"];
      struct Template { std::vector<std::pair<uint32_t, uint32_t>> parts; bool taxi; float length; };
      std::vector<Template> templates;
      const json carsJson = tj.value("carros", json::array());
      for (const json& cf : carsJson) {
        std::string file = cf.get<std::string>();
        if (!std::ifstream(file) && std::ifstream(sceneDir + file)) file = sceneDir + file;
        const uint32_t first = uint32_t(scene.objects.size());
        if (!scene::loadGltf(scene, file, glm::mat4(1.0f))) {
          LOG_WARN("Trânsito: não carreguei %s", file.c_str());
          continue;
        }
        Template t{{}, file.find("taxi") != std::string::npos, 4.8f};
        for (uint32_t o = first; o < scene.objects.size(); ++o) {
          t.parts.push_back({scene.objects[o].mesh, scene.objects[o].material});
          scene.setObjectHidden(o, true);  // o original fica escondido
        }
        if (file.find("van") != std::string::npos) t.length = 5.3f;
        if (file.find("hatch") != std::string::npos) t.length = 4.1f;
        if (file.find("onibus") != std::string::npos) t.length = 12.2f;
        templates.push_back(t);
      }
      // Semáforo: cada faixa pode ter "paradas" (distâncias, em m desde o início da faixa, das linhas de parada) e
      // uma "fase" (0 ou 1): a fase 0 tem verde na primeira metade do "ciclo" de "semaforo", a fase 1 na segunda.
      struct Lane { glm::vec2 a, d; float length; std::vector<float> stops; int phase; };
      std::vector<Lane> lanes;
      const json lanesJson = tj.value("faixas", json::array());
      for (const json& fj : lanesJson) {
        const glm::vec2 a(fj["de"][0].get<float>(), fj["de"][1].get<float>()), b(fj["ate"][0].get<float>(), fj["ate"][1].get<float>());
        const float len = glm::length(b - a);
        if (len <= 5.0f) continue;
        Lane l{a, (b - a) / len, len, {}, fj.value("fase", 0)};
        const json stops = fj.value("paradas", json::array());
        for (const json& st : stops) l.stops.push_back(st.get<float>());
        std::sort(l.stops.begin(), l.stops.end());
        lanes.push_back(l);
      }
      const float cycle = std::max(10.0f, tj.value("semaforo", 50.0f));
      if (!templates.empty() && !lanes.empty()) {
        std::mt19937 rng(tj.value("semente", 1u));
        std::uniform_real_distribution<float> u(0.0f, 1.0f);
        const json vel = tj.value("velocidade", json::array({5.0, 11.0}));
        const float vmin = vel[0].get<float>(), vmax = vel[1].get<float>(), y = tj.value("altura", 0.0f);
        static const glm::vec3 paints[] = {{0.02f, 0.02f, 0.022f}, {0.85f, 0.85f, 0.86f}, {0.45f, 0.46f, 0.48f},
                                           {0.25f, 0.26f, 0.28f}, {0.05f, 0.08f, 0.2f},  {0.45f, 0.04f, 0.04f},
                                           {0.06f, 0.15f, 0.08f}, {0.6f, 0.58f, 0.52f}};
        struct Car { uint32_t lane; float s, speed, vmax, len; std::vector<uint32_t> objs; };
        auto cars = std::make_shared<std::vector<Car>>();
        const int count = std::max(0, tj.value("quantidade", 60));
        for (int i = 0; i < count; ++i) {
          const Template& t = templates[size_t(u(rng) * float(templates.size())) % templates.size()];
          Car c{uint32_t(u(rng) * float(lanes.size())) % uint32_t(lanes.size()), 0.0f, 0.0f,
                vmin + (vmax - vmin) * u(rng), t.length, {}};
          c.s = u(rng) * lanes[c.lane].length;
          c.speed = c.vmax;
          const glm::vec3 paint = paints[size_t(u(rng) * 8.0f) % 8];
          for (auto [mesh, mat] : t.parts) {
            scene::Material m = scene.materials[mat];
            const bool body = (m.flags & scene::kMaterialClearcoat) && glm::dot(m.emissive, m.emissive) == 0.0f;
            uint32_t useMat = mat;
            if (body && !t.taxi) {
              m.baseColor = glm::vec4(paint, 1.0f);
              useMat = scene.addMaterial(m);
            }
            c.objs.push_back(scene.addObject(mesh, glm::mat4(1.0f), useMat));
          }
          cars->push_back(std::move(c));
        }
        const size_t laneCount = lanes.size();
        scene.addAnimator([cars, lanes, laneCount, y, cycle](scene::Scene& sc, float time, float dt) {
          std::vector<Car>& C = *cars;
          const float step = std::min(dt, 0.1f);
          const bool firstHalf = std::fmod(time, cycle) < cycle * 0.5f;
          // Por faixa, em ordem de posição: cada carro freia se o da frente estiver perto (2 s de distância + 3 m).
          std::vector<std::vector<uint32_t>> byLane(laneCount);
          for (uint32_t i = 0; i < C.size(); ++i) byLane[C[i].lane].push_back(i);
          for (auto& list : byLane) {
            std::sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) { return C[a].s < C[b].s; });
            for (size_t k = 0; k < list.size(); ++k) {
              Car& c = C[list[k]];
              const Car& ahead = C[list[(k + 1) % list.size()]];
              float gap = ahead.s - c.s;
              if (gap <= 0.0f) gap += lanes[c.lane].length;  // o da frente já deu a volta
              if (list.size() == 1) gap = 1e9f;
              float want = gap < 3.0f + c.len + c.speed * 2.0f ? std::max(0.0f, (gap - c.len - 2.5f) / 2.0f) : c.vmax;
              const Lane& L = lanes[c.lane];
              if (!L.stops.empty() && (firstHalf == (L.phase != 0))) {  // vermelho: para antes da próxima linha
                for (float st : L.stops) {
                  const float toStop = st - (c.s + c.len * 0.5f);
                  if (toStop < -0.5f) continue;
                  if (toStop < 6.0f + c.speed * c.speed / 6.0f) want = std::min(want, std::max(0.0f, toStop) / 1.5f);
                  break;
                }
              }
              c.speed += glm::clamp(want - c.speed, -6.0f * step, 2.5f * step);  // freia forte, acelera devagar
            }
          }
          for (Car& c : C) {
            const Lane& L = lanes[c.lane];
            c.s = std::fmod(c.s + c.speed * step, L.length);
            const glm::vec2 p = L.a + L.d * c.s;
            glm::mat4 m = glm::translate(glm::mat4(1.0f), {p.x, y, p.y});
            m = glm::rotate(m, std::atan2(L.d.x, L.d.y), glm::vec3(0, 1, 0));
            for (uint32_t o : c.objs) sc.setTransform(o, m);
          }
        });
        LOG_INFO("Trânsito: %d carros (%zu modelos) em %zu faixas", count, templates.size(), lanes.size());
      }
    }
    // Terreno de quilômetros: ladrilhos de "ladrilho" metros (o padrão é a célula do mundo aberto), cada um uma
    // entidade parada com a malha detalhada e colisão; no mundo aberto, também a malha simples de longe.
    if (doc.contains("terreno") && doc["terreno"].is_object()) {
      const json& tj = doc["terreno"];
      Terrain& T = terrain;
      T.enabled = true;
      T.size = std::max(8.0f, tj.value("tamanho", T.size));
      T.tile = std::max(4.0f, tj.value("ladrilho", openWorld.enabled ? openWorld.cellSize : T.tile));
      T.height = tj.value("altura", T.height);
      T.base = tj.value("base", T.base);
      T.noiseScale = std::max(1.0f, tj.value("escala", T.noiseScale));
      T.octaves = std::clamp(tj.value("oitavas", T.octaves), 1, 12);
      T.seed = float(tj.value("semente", 0)) * 37.17f;
      if (tj.contains("centro")) T.center = glm::vec2(tj["centro"][0].get<float>(), tj["centro"][1].get<float>());
      if (tj.contains("mapa")) {
        std::string mp = tj["mapa"].get<std::string>();
        if (!std::ifstream(mp) && std::ifstream(sceneDir + mp)) mp = sceneDir + mp;
        int w = 0, h = 0, n = 0;
        std::ifstream raw(mp, std::ios::binary);
        char magic[4] = {};
        float hdr[5] = {};
        if (mp.size() > 7 && mp.substr(mp.size() - 7) == ".altura" && raw.read(magic, 4) && std::memcmp(magic, "LGAL", 4) == 0 &&
            raw.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) {
          // Salvo pelo pincel do editor (salvar_terreno): alturas em metros, tamanho e centro exatos.
          T.mapW = int(hdr[0]);
          T.mapH = int(hdr[1]);
          T.size = hdr[2];
          T.center = glm::vec2(hdr[3], hdr[4]);
          T.base = 0.0f;
          T.height = 1.0f;
          T.map.resize(size_t(T.mapW) * size_t(T.mapH));
          raw.read(reinterpret_cast<char*>(T.map.data()), std::streamsize(T.map.size() * sizeof(float)));
        } else if (stbi_us* px = stbi_load_16(mp.c_str(), &w, &h, &n, 1)) {
          T.map.resize(size_t(w) * size_t(h));
          for (size_t i = 0; i < T.map.size(); ++i) T.map[i] = float(px[i]) / 65535.0f;
          T.mapW = w;
          T.mapH = h;
          stbi_image_free(px);
        } else {
          LOG_WARN("Terreno: não li o mapa de altura %s; usando ruído", mp.c_str());
        }
      }
      const uint32_t res = std::clamp(tj.value("resolucao", 64u), 2u, 1024u);  // quadrados por lado do ladrilho
      const uint32_t farRes = std::max(2u, res / 8u);
      T.res = res;
      T.farRes = farRes;
      const std::string tm = tj.value("material", "");
      auto mit = materials.find(tm);
      if (mit == materials.end()) mit = materials.find("");
      const float friction = tj.value("atrito", 0.8f);
      const int n = std::max(1, int(std::ceil(T.size / T.tile)));
      const float span = float(n) * T.tile;
      uint32_t tiles = 0, tris = 0;
      for (int tz = 0; tz < n; ++tz)
        for (int tx = 0; tx < n; ++tx) {
          const float cx = T.center.x - 0.5f * span + (float(tx) + 0.5f) * T.tile;
          const float cz = T.center.y - 0.5f * span + (float(tz) + 0.5f) * T.tile;
          scene::MeshData nearMesh = terrainTileMesh(cx, cz, res, 0.02f * T.tile);
          tris += uint32_t(nearMesh.indices.size() / 3);
          const glm::mat4 xf = glm::translate(glm::mat4(1.0f), glm::vec3(cx, 0.0f, cz));
          const entt::entity ent = registry.create();
          Transform t;
          t.position = glm::vec3(cx, terrainHeight(cx, cz), cz);
          registry.emplace<Transform>(ent, t);
          registry.emplace<Renderable>(ent, scene.addObject(scene.addMesh(std::move(nearMesh)), xf, mit->second), tm);
          TerrainTile tt;
          tt.friction = friction;
          tt.center = glm::vec2(cx, cz);
          if (openWorld.enabled) {
            tt.farObject = scene.addObject(scene.addMesh(terrainTileMesh(cx, cz, farRes, 0.08f * T.tile)), xf, mit->second);
            scene.setObjectHidden(tt.farObject, true);
          }
          registry.emplace<TerrainTile>(ent, tt);
          ++tiles;
        }
      LOG_INFO("Terreno: %.0f m de lado em %u ladrilhos de %.0f m (%u triângulos de perto)%s", span, tiles, T.tile, tris,
               T.mapW ? " pelo mapa de altura" : "");
    }
    // Linhas do tempo (no estilo do Sequencer): cada uma vira um script de sistema que monta a sequência (biblioteca
    // scripts/lib/sequencia.lua) com as faixas do JSON e toca ao iniciar ("tocar_ao_iniciar") ou quando um script
    // chama tocar_linha("nome").
    if (doc.contains("linhas_do_tempo") && doc["linhas_do_tempo"].is_object()) {
      std::function<std::string(const json&)> lua = [&](const json& v) -> std::string {
        if (v.is_object()) {
          std::string o = "{";
          for (auto& [k, x] : v.items()) o += "[" + json(k).dump() + "] = " + lua(x) + ", ";
          return o + "}";
        }
        if (v.is_array()) {
          std::string o = "{";
          for (const json& x : v) o += lua(x) + ", ";
          return o + "}";
        }
        if (v.is_null()) return "nil";
        return v.dump();  // número, booleano e texto (as aspas e escapes do JSON valem no Lua)
      };
      std::error_code ec;
      const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "legacy_linhas_do_tempo";
      std::filesystem::create_directories(dir, ec);
      for (auto& [name, def] : doc["linhas_do_tempo"].items()) {
        json d = def;
        const bool autoplay = d.value("tocar_ao_iniciar", false);
        d.erase("tocar_ao_iniciar");
        const std::filesystem::path out =
            dir / (std::to_string(std::hash<std::string>{}(path + "/" + name)) + ".lua");
        std::ofstream(out, std::ios::binary)
            << "-- Linha do tempo \"" << name << "\" (gerada da cena; não edite)\n"
            << "incluir(\"lib/sequencia.lua\")\n"
            << "local s = sequencia.nova(" << lua(d) << ")\n"
            << "s.ao_terminar = function() log(\"LINHA\", \"fim " << name << "\") end\n"
            << "function iniciar(e)" << (autoplay ? " s:tocar()" : "") << " end\n"
            << "function atualizar(e, dt)\n  if linha_pedida(" << json(name).dump() << ") then s:tocar() end\n"
            << "  s:atualizar(dt)\nend\n";
        const entt::entity ent = registry.create();
        registry.emplace<Name>(ent, "_linha_" + name);
        registry.emplace<SystemEntity>(ent);
        registry.emplace<Script>(ent, out.string());
        LOG_INFO("Linha do tempo %s: %zu faixas de objetos, %zu chaves de câmera, %zu eventos", name.c_str(),
                 d.value("objetos", json::array()).size(), d.value("camera", json::array()).size(),
                 d.value("eventos", json::array()).size());
      }
    }
    // Menu de pausa (Esc / Start): o padrão do motor, outro script ("menu": "caminho.lua") ou nenhum ("menu": false).
    // Roda numa entidade de sistema (sem forma, fora do arquivo salvo e do editor).
    if (const json menu = doc.contains("menu") ? doc["menu"] : json("scripts/menu_padrao.lua"); menu.is_string()) {
      std::string mpath = menu.get<std::string>();
      if (!std::ifstream(mpath) && std::ifstream(sceneDir + mpath)) mpath = sceneDir + mpath;
      if (std::ifstream(mpath)) {
        const entt::entity ent = registry.create();
        registry.emplace<Name>(ent, "_menu");
        registry.emplace<SystemEntity>(ent);
        registry.emplace<Script>(ent, mpath);
      } else if (doc.contains("menu")) {
        LOG_WARN("Cena: menu %s não encontrado", mpath.c_str());
      }
    }
    // Explosões de um tiro: 4 emissores reservados só se algum script da cena chama explosao().
    for (auto ent : registry.view<Script>()) {
      std::ifstream sf(registry.get<Script>(ent).path);
      const std::string text((std::istreambuf_iterator<char>(sf)), std::istreambuf_iterator<char>());
      if (text.find("explosao(") != std::string::npos) {
        for (int i = 0; i < 4; ++i) explosionSlots.push_back(scene.addExplosionSlot(1500));
        break;
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
  // "no_chao": antes de criar corpos, personagens e a navegação, a altura passa a contar do chão do terreno.
  if (terrain.enabled) {
    for (auto ent : registry.view<OnGround, Transform>()) {
      Transform& t = registry.get<Transform>(ent);
      t.position.y += terrainHeight(t.position.x, t.position.z);
      if (const Renderable* r = registry.try_get<Renderable>(ent); r && loadedScene_ && registry.all_of<Shape>(ent))
        loadedScene_->setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
      if (Character* c = registry.try_get<Character>(ent)) {
        for (glm::vec3& p : c->aiPatrol) p.y += terrainHeight(p.x, p.z);
        c->aiTarget.y += terrainHeight(c->aiTarget.x, c->aiTarget.z);
      }
    }
  }
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
      // Milhões de triângulos (a cidade): segundos montando a árvore do Jolt. Numa thread à parte, com a tela de
      // carregamento viva na principal.
      carregamento::emParalelo([&] { m.bodyId = physics->addStaticMesh(verts, idx, m.friction); }, 1);
      LOG_INFO("Física: colisão de %s com %zu triângulos", m.path.c_str(), idx.size() / 3);
    }
  }
  // Terreno: a malha detalhada de cada ladrilho, no mundo, vira um corpo parado.
  for (auto ent : registry.view<TerrainTile, Renderable>()) {
    TerrainTile& tt = registry.get<TerrainTile>(ent);
    if (!loadedScene_) break;
    const scene::Object& obj = loadedScene_->objects[registry.get<Renderable>(ent).sceneObject];
    const scene::MeshData& mesh = loadedScene_->cpuMesh(obj.mesh);
    std::vector<glm::vec3> verts;
    verts.reserve(mesh.vertices.size());
    for (const scene::Vertex& v : mesh.vertices) verts.push_back(glm::vec3(obj.transform * glm::vec4(v.position, 1.0f)));
    tt.bodyId = physics->addStaticMesh(verts, mesh.indices, tt.friction);
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
  if (openWorld.enabled) {
    partitionOpenWorld();
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
    for (auto ent : registry.view<TerrainTile, Renderable>()) {
      if (!loadedScene_) break;
      const scene::Object& obj = loadedScene_->objects[registry.get<Renderable>(ent).sceneObject];
      const scene::MeshData& mesh = loadedScene_->cpuMesh(obj.mesh);
      const uint32_t base = uint32_t(verts.size());
      for (const scene::Vertex& v : mesh.vertices) verts.push_back(glm::vec3(obj.transform * glm::vec4(v.position, 1.0f)));
      for (uint32_t i : mesh.indices) idx.push_back(base + i);
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
  if (networkStep) networkStep(dt);
  realTime += dt;
  if (paused) {  // jogo pausado: só os scripts (o menu); física, personagens e o tempo de jogo parados
    if (scripting) scripting->update(scene, dt);
    return;
  }
  time += dt;
  if (!netClient) updateOpenWorld(scene);
  if (scripting) scripting->update(scene, dt);
  if (paused) return;  // um script pausou agora: a pausa já vale neste quadro (nada se move)
  if (netClient) {  // cliente de rede: as poses vêm do anfitrião (game/network); aqui só a animação dos "visual"
    // Previsão: o boneco deste jogador anda já com a entrada local (o anfitrião confirma e o network corrige).
    if (predictLocal && localSlot >= 0) {
      const entt::entity e = slotEntity(localSlot);
      if (e != entt::null) {
        Character& c = registry.get<Character>(e);
        if (c.id != ~0u) {
          const glm::vec3 fwd(std::cos(input_.cameraYaw), 0.0f, std::sin(input_.cameraYaw));
          const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
          glm::vec3 dir = fwd * input_.move.y + right * input_.move.x;
          if (glm::dot(dir, dir) > 1.0f) dir = glm::normalize(dir);
          physics->moveCharacter(c.id, dir * (input_.run ? c.runSpeed : c.walkSpeed), input_.jump, c.jumpSpeed);
          physics->stepCharacterOnly(c.id, dt);
          Transform& t = registry.get<Transform>(e);
          t.position = physics->characterFeet(c.id) + glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
          if (const Renderable* r = registry.try_get<Renderable>(e); r && registry.all_of<Shape>(e))
            scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(e)));
        }
      }
    }
    for (auto ent : registry.view<Transform, Character>()) {
      Character& c = registry.get<Character>(ent);
      if (!c.visual.enabled || c.visual.model < 0) continue;
      const glm::vec3 pos = registry.get<Transform>(ent).position;
      if (c.netHasLast) c.netVelocity = glm::mix(c.netVelocity, (pos - c.netLastPos) / dt, 1.0f - std::exp(-dt * 15.0f));
      c.netLastPos = pos;
      c.netHasLast = true;
      ragdollBlendStep(scene, c, dt);
      animateCharacter(scene, c, pos - glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f), dt);
    }
    return;
  }
  // Personagens: velocidade desejada pela entrada do jogador ou pelo roteiro.
  for (auto ent : registry.view<Character>()) {
    Character& c = registry.get<Character>(ent);
    if (c.id == ~0u) continue;
    if (c.ragdoll.active) {  // a cápsula fica guardada fora do mapa enquanto o corpo é da física
      physics->setCharacterFeet(c.id, glm::vec3(0.0f, -3000.0f - 10.0f * float(entt::to_integral(ent) & 1023u), 0.0f));
      continue;
    }
    glm::vec3 dir(0.0f);
    bool jump = false, run = false;
    if (c.remoteSlot >= 0) {  // vaga de jogador remoto
      if (size_t(c.remoteSlot) >= slotActive.size() || !slotActive[size_t(c.remoteSlot)]) {
        physics->setCharacterFeet(c.id, glm::vec3(0.0f, -1000.0f - 10.0f * float(c.remoteSlot), 0.0f));  // estacionada
        continue;
      }
      const PlayerInput& in = remoteInputs[size_t(c.remoteSlot)];
      const glm::vec3 fwd(std::cos(in.cameraYaw), 0.0f, std::sin(in.cameraYaw));
      const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
      dir = fwd * in.move.y + right * in.move.x;
      if (glm::dot(dir, dir) > 1.0f) dir = glm::normalize(dir);
      jump = in.jump;
      run = in.run;
    } else if (c.player && !playerControl) {
      // Cutscene: parado (a física continua: cai, é empurrado).
    } else if (c.player) {
      // Frente = para onde a câmera olha, no plano do chão.
      const glm::vec3 fwd(std::cos(input_.cameraYaw), 0.0f, std::sin(input_.cameraYaw));
      const glm::vec3 right(-fwd.z, 0.0f, fwd.x);
      dir = fwd * input_.move.y + right * input_.move.x;
      if (glm::dot(dir, dir) > 1.0f) dir = glm::normalize(dir);
      jump = input_.jump;
      run = input_.run;
    } else if (c.ai && c.aiHasTarget && navigation) {
      const glm::vec3 feet = physics->characterFeet(c.id);
      auto feetOf = [&](entt::entity e) {
        glm::vec3 p = registry.get<Transform>(e).position;
        if (const Character* oc = registry.try_get<Character>(e)) p.y -= 0.5f * oc->desc.height;
        return p;
      };
      // Decide o objetivo deste quadro (prioridade: fugir > perseguir > procurar > seguir > patrulhar > ir ao alvo).
      glm::vec3 goal = c.aiTarget;
      float stop = c.aiStopDistance;
      const std::string before = c.aiState;
      c.aiState = "parado";
      bool have = false;
      if (const entt::entity threat = c.aiFlee.empty() ? entt::null : findByName(c.aiFlee);
          threat != entt::null && sees(c, threat)) {
        c.aiThreat = feetOf(threat);
        c.aiFleeTime = 3.0f;  // continua fugindo mesmo de costas para ela
      }
      if (c.aiFleeTime > 0.0f) {
        c.aiFleeTime -= dt;
        glm::vec3 away = feet - c.aiThreat;
        away.y = 0.0f;
        away = glm::length(away) > 1e-3f ? glm::normalize(away) : glm::vec3(1.0f, 0.0f, 0.0f);
        goal = feet + away * 8.0f;  // a navegação leva ao ponto alcançável mais perto
        stop = 0.5f;
        c.aiState = "fugindo";
        run = true;
        have = true;
      }
      if (!have && !c.aiChase.empty()) {
        if (const entt::entity prey = findByName(c.aiChase); prey != entt::null && sees(c, prey)) {
          goal = c.aiLastSeen = feetOf(prey);
          c.aiSearchTime = 4.0f;  // perdeu de vista: procura no último lugar por 4 s
          c.aiState = "perseguindo";
          run = true;
          have = true;
        } else if (c.aiSearchTime > 0.0f) {
          c.aiSearchTime -= dt;
          goal = c.aiLastSeen;
          stop = 0.6f;
          c.aiState = "procurando";
          have = true;
          if (glm::length(glm::vec2(goal.x - feet.x, goal.z - feet.z)) < 0.7f) c.aiSearchTime = std::min(c.aiSearchTime, 1.0f);
        }
      }
      if (!have && !c.aiFollow.empty()) {
        if (const entt::entity e = findByName(c.aiFollow); e != entt::null) {
          goal = feetOf(e);
          c.aiState = "seguindo";
          have = true;
        }
      }
      if (!have && !c.aiPatrol.empty()) {
        goal = c.aiPatrol[c.aiPatrolIdx % c.aiPatrol.size()];
        stop = 0.5f;
        if (glm::length(glm::vec2(goal.x - feet.x, goal.z - feet.z)) < 0.6f) {
          c.aiPatrolIdx = (c.aiPatrolIdx + 1) % c.aiPatrol.size();
          goal = c.aiPatrol[c.aiPatrolIdx];
        }
        c.aiState = "patrulhando";
        have = true;
      }
      if (!have && c.aiChase.empty() && c.aiFlee.empty() && c.aiPatrol.empty()) {  // "alvo" ou e:ir_para
        c.aiState = "indo";
        have = true;
      }
      // Caminho recalculado a cada 0,5 s (o alvo pode andar), quando acabou ou quando o comportamento mudou.
      c.aiRepath -= dt;
      const bool goalMoved = !c.aiHasGoal || glm::length(goal - c.aiGoal) > 1.0f || c.aiState != before;
      if (have && (c.aiRepath <= 0.0f || c.aiNext >= c.aiPath.size() || goalMoved)) {
        c.aiRepath = 0.5f;
        c.aiGoal = goal;
        c.aiHasGoal = true;
        navigation->findPath(feet, goal, c.aiPath);
        c.aiNext = c.aiPath.size() > 1 ? 1 : c.aiPath.size();
      }
      const glm::vec2 toGoal(goal.x - feet.x, goal.z - feet.z);
      if (have && glm::length(toGoal) > stop) {
        while (c.aiNext < c.aiPath.size() &&
               glm::length(glm::vec2(c.aiPath[c.aiNext].x - feet.x, c.aiPath[c.aiNext].z - feet.z)) < 0.25f)
          ++c.aiNext;
        if (c.aiNext < c.aiPath.size()) {
          const glm::vec3 d(c.aiPath[c.aiNext].x - feet.x, 0.0f, c.aiPath[c.aiNext].z - feet.z);
          if (glm::length(d) > 1e-3f) dir = glm::normalize(d);
        }
      } else if (c.aiState == "indo") {
        c.aiState = "parado";
      }
      if (glm::length(dir) > 1e-3f) c.aiFacing = dir;  // para onde ele olha (cone de visão)
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
  for (auto ent : registry.view<Transform, Shape, Character>()) {
    auto& t = registry.get<Transform>(ent);
    Character& c = registry.get<Character>(ent);
    if (c.id == ~0u) continue;
    ragdollBlendStep(scene, c, dt);
    if (c.ragdoll.active && c.visual.model >= 0) {
      scene::Model& m = scene.models[size_t(c.visual.model)];
      for (size_t k = 0; k < c.ragdoll.bodies.size(); ++k) {
        glm::vec3 p;
        glm::quat q;
        physics->pose(c.ragdoll.bodies[k], p, q);
        m.drivenWorld[k] = glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q) * c.ragdoll.offset[k];
      }
      // Ragdoll ativo: alvos dos motores pela pose pura da animação (relativa entre as partes: a raiz se cancela).
      m.keepAnimPose = c.ragdoll.strength > 0.0f && c.ragdoll.followAnimation;
      if (c.ragdoll.strength > 0.0f || c.ragdoll.motorsOn) {
        std::vector<glm::quat> targets;
        if (c.ragdoll.strength > 0.0f) targets = ragdollTargets(m, c.ragdoll);
        physics->setRagdollMotors(c.ragdoll.bodies, targets, targets.empty() ? 0.0f : c.ragdoll.strength);
        c.ragdoll.motorsOn = !targets.empty();
      }
      // A origem do modelo acompanha a bacia (a caixa de recorte do desenho fica no lugar certo).
      const glm::vec3 pelvis(m.drivenWorld[0][3]);
      m.root = glm::translate(glm::mat4(1.0f), pelvis - c.ragdoll.rootToPelvis) * c.ragdoll.rootBasis;
      t.position = pelvis;
      if (const Renderable* r = registry.try_get<Renderable>(ent))
        scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
      continue;
    }
    const glm::vec3 feet = physics->characterFeet(c.id);
    t.position = feet + glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
    if (const Renderable* r = registry.try_get<Renderable>(ent))
      scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
    if (c.visual.enabled && c.visual.model >= 0) animateCharacter(scene, c, feet, dt);
  }
  // Só os corpos que se movem mudam a pose; parados (dormindo) não reenviam nada à GPU.
  auto view = registry.view<Transform, Shape, RigidBody, Renderable>();
  for (auto ent : view) {
    auto [t, s, b, r] = view.get<Transform, Shape, RigidBody, Renderable>(ent);
    if (b.bodyId == ~0u || b.desc.motion == Motion::Static || !physics->isActive(b.bodyId)) continue;
    physics->pose(b.bodyId, t.position, t.rotation);
    scene.setTransform(r.sceneObject, drawMatrix(t, s));
  }
  for (auto ent : registry.view<Transform, ParticleSource>()) {  // partículas acompanham a entidade
    const ParticleSource& ps = registry.get<ParticleSource>(ent);
    const Transform& t = registry.get<Transform>(ent);
    scene.setEmitter(ps.emitter, t.position + t.rotation * ps.offset, ps.enabled ? ps.scale : 0.0f);
  }
}

void World::ragdollBlendStep(scene::Scene& scene, Character& c, float dt) {
  if (c.ragdoll.active || c.ragdoll.blendOut <= 0.0f || c.visual.model < 0) return;
  scene::Model& m = scene.models[size_t(c.visual.model)];
  c.ragdoll.blendOut = std::max(c.ragdoll.blendOut - dt, 0.0f);
  const float x = c.ragdoll.blendOut / kRagdollBlendOut;
  m.drivenWeight = x * x * (3.0f - 2.0f * x);
  if (c.ragdoll.blendOut <= 0.0f) {
    m.driven.clear();
    m.drivenWorld.clear();
    m.drivenWeight = 1.0f;
  }
}

void World::animateCharacter(scene::Scene& scene, Character& c, glm::vec3 feet, float dt) {
  CharacterVisual& v = c.visual;
  scene::Model& m = scene.models[size_t(v.model)];
  // Rosto: as expressões pedidas e o piscar (a cada 2–6 s, 0,16 s fechando e abrindo; às vezes duas seguidas).
  {
    v.blinkTimer -= dt;
    if (v.blinkTimer <= 0.0f) {
      v.blinkTime = 0.0f;
      const float r = float(std::rand()) / float(RAND_MAX);
      v.blinkTimer = r < 0.15f ? 0.35f : 2.0f + 4.0f * r;
    }
    float blink = 0.0f;
    if (v.blinkTime >= 0.0f) {
      v.blinkTime += dt;
      blink = v.blinkTime < 0.16f ? std::sin(glm::pi<float>() * v.blinkTime / 0.16f) : 0.0f;
      if (v.blinkTime >= 0.16f) v.blinkTime = -1.0f;
    }
    for (const auto& [name, w] : v.expressions) scene.setMorphWeight(v.model, name, w);
    for (const char* eye : {"eye-left-closure", "eye-right-closure"}) {
      const auto it = v.expressions.find(eye);
      scene.setMorphWeight(v.model, eye, std::min(1.0f, (it == v.expressions.end() ? 0.0f : it->second) + blink));
    }
  }
  // Cabelo por fios: a calota segue o osso da cabeça (pose do quadro anterior; a simulação dos fios cuida da inércia).
  if (v.hairObject >= 0 && v.hairNode >= 0 && size_t(v.hairNode) < m.nodeWorld.size())
    scene.setTransform(uint32_t(v.hairObject), m.nodeWorld[size_t(v.hairNode)] * glm::translate(glm::mat4(1.0f), v.hairCenter) *
                                                   glm::scale(glm::mat4(1.0f), v.hairRadius));
  // No cliente de rede a física não roda: a velocidade vem das poses recebidas e o personagem conta como no chão
  // enquanto não sobe nem desce rápido.
  const glm::vec3 vel = netClient ? c.netVelocity : physics->characterVelocity(c.id);
  const float hs = glm::length(glm::vec2(vel.x, vel.z));
  const bool onGround = netClient ? std::abs(vel.y) < 1.0f : physics->characterOnGround(c.id);
  // Velocidade e direção suavizadas (sem trancos de um quadro para o outro).
  const float k = 1.0f - std::exp(-dt * 10.0f);
  v.speed += ((onGround ? hs : v.speed) - v.speed) * k;
  if (hs > 0.2f) {
    const float want = std::atan2(vel.x, vel.z);
    float d = std::remainder(want - v.yaw, glm::two_pi<float>());
    v.yaw += d * (1.0f - std::exp(-dt * 12.0f));
  }
  // Locomoção: mistura parado/andar/correr; andar e correr com a mesma fase (os pés batem juntos na transição) e
  // tocados na velocidade real (s / velocidade de referência), sem o pé patinar no chão.
  float wIdle, wWalk, wRun;
  locomotionWeights(v, v.speed, wIdle, wWalk, wRun);
  auto duration = [&](int a) { return a >= 0 ? std::max(m.animations[size_t(a)].duration, 1e-3f) : 1.0f; };
  float rate = 0.0f, moving = wWalk + wRun;
  if (moving > 0.0f) {
    if (v.walkA >= 0) rate += wWalk * glm::clamp(v.speed / v.walkRef, 0.25f, 2.0f) / duration(v.walkA);
    if (v.runA >= 0) rate += wRun * glm::clamp(v.speed / v.runRef, 0.25f, 2.0f) / duration(v.runA);
    rate /= moving;
  }
  v.phase = std::fmod(v.phase + dt * rate, 1.0f);
  v.idleTime += dt;
  // Ação pedida por script (e:animar): entra e sai com transição; sem laço, sai sozinha no fim.
  if (v.action >= 0) {
    v.actionTime += dt;
    if (!v.actionLoop && v.actionTime >= duration(v.action) - v.actionFade) v.actionOn = false;
    const float step = dt / std::max(v.actionFade, 1e-3f);
    v.actionWeight = glm::clamp(v.actionWeight + (v.actionOn ? step : -step), 0.0f, 1.0f);
    if (!v.actionOn && v.actionWeight <= 0.0f) v.action = -1;
  }
  const float loco = 1.0f - (v.action >= 0 ? v.actionWeight : 0.0f);
  m.layers.clear();
  if (wIdle > 0.0f) m.layers.push_back({v.idleA, v.idleTime, wIdle * loco});
  if (wWalk > 0.0f) m.layers.push_back({v.walkA, v.phase * duration(v.walkA), wWalk * loco});
  if (wRun > 0.0f) m.layers.push_back({v.runA, v.phase * duration(v.runA), wRun * loco});
  if (v.action >= 0) m.layers.push_back({v.action, v.actionTime, v.actionWeight});
  if (m.layers.empty()) m.layers.push_back({-1, 0.0f, 1.0f});
  placeVisual(m, v, feet);
  // IK dos pés: só no chão (no ar, as pernas seguem a animação), entrando e saindo devagar.
  const float ikTarget = (v.ik && onGround) ? 1.0f : 0.0f;
  m.ikWeight += (ikTarget - m.ikWeight) * (1.0f - std::exp(-dt * 8.0f));
  m.maxPelvisDrop = 0.5f * c.desc.height;
  if (!m.ground && !m.legs.empty()) {
    const uint32_t id = c.id;
    const float step = c.desc.stepHeight;
    Physics* ph = physics.get();
    m.ground = [ph, id, step](glm::vec3 from, float& groundY) {
      const float above = step + 0.2f;  // começa acima de um degrau que o personagem consegue subir
      float dist = 0.0f;
      if (!ph->castRayIgnoringCharacter(from + glm::vec3(0.0f, above, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                                        above + 1.0f, id, &dist))
        return false;
      groundY = from.y + above - dist;
      return true;
    };
  }
}

namespace {
std::string savePath(const std::string& name) {
  std::string clean;
  for (char ch : name)
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-') clean += ch;
  if (clean.empty()) clean = "jogo";
  return "saves/" + clean + ".json";
}
}  // namespace

bool World::gameExists(const std::string& name) { return bool(std::ifstream(savePath(name))); }

bool World::saveGame(const std::string& name, const std::string& dataJson) const {
  json doc;
  doc["versao"] = 1;
  doc["tempo"] = time;
  json ents = json::array();
  for (auto ent : registry.view<Name, Transform>()) {
    if (registry.all_of<SystemEntity>(ent)) continue;
    const Transform& t = registry.get<Transform>(ent);
    json e;
    e["nome"] = registry.get<Name>(ent).value;
    if (const Character* c = registry.try_get<Character>(ent); c && c->id != ~0u) {
      e["pes"] = toJson(physics->characterFeet(c->id));
      e["velocidade"] = toJson(physics->characterVelocity(c->id));
    } else if (const Vehicle* v = registry.try_get<Vehicle>(ent); v && v->id != ~0u) {
      const uint32_t b = physics->vehicleBody(v->id);
      e["posicao"] = toJson(t.position);
      e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
      e["velocidade"] = toJson(physics->linearVelocity(b));
      e["velocidade_angular"] = toJson(physics->angularVelocity(b));
    } else if (const RigidBody* rb = registry.try_get<RigidBody>(ent); rb && rb->bodyId != ~0u && rb->desc.motion != Motion::Static) {
      e["posicao"] = toJson(t.position);
      e["rotacao"] = json::array({t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z});
      e["velocidade"] = toJson(physics->linearVelocity(rb->bodyId));
      e["velocidade_angular"] = toJson(physics->angularVelocity(rb->bodyId));
    } else {
      continue;  // parado: vem da própria cena
    }
    ents.push_back(e);
  }
  doc["entidades"] = ents;
  try {
    doc["dados"] = dataJson.empty() ? json() : json::parse(dataJson);
  } catch (const json::exception&) {
    doc["dados"] = json();
  }
  std::error_code ec;
  std::filesystem::create_directories("saves", ec);
  const std::string path = savePath(name);
  std::ofstream f(path);
  if (!f) {
    LOG_ERROR("Não consegui gravar %s", path.c_str());
    return false;
  }
  f << doc.dump(1);
  LOG_INFO("Jogo salvo em %s (%zu entidades)", path.c_str(), ents.size());
  return bool(f);
}

bool World::loadGame(const std::string& name, std::string& dataJson) {
  const std::string path = savePath(name);
  std::ifstream f(path);
  if (!f) return false;
  json doc;
  try {
    doc = json::parse(f);
  } catch (const json::exception& e) {
    LOG_ERROR("%s inválido (%s)", path.c_str(), e.what());
    return false;
  }
  std::map<std::string, entt::entity> byName;
  for (auto ent : registry.view<Name>()) byName[registry.get<Name>(ent).value] = ent;
  size_t applied = 0;
  for (const json& e : doc.value("entidades", json::array())) {
    auto it = byName.find(e.value("nome", ""));
    if (it == byName.end()) continue;
    const entt::entity ent = it->second;
    const glm::vec3 vel = vec3Of(e.value("velocidade", json()), glm::vec3(0.0f));
    const glm::vec3 ang = vec3Of(e.value("velocidade_angular", json()), glm::vec3(0.0f));
    glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
    if (e.contains("rotacao")) {
      const json& q = e["rotacao"];
      rot = glm::normalize(glm::quat(q[0].get<float>(), q[1].get<float>(), q[2].get<float>(), q[3].get<float>()));
    }
    if (Character* c = registry.try_get<Character>(ent); c && c->id != ~0u && e.contains("pes")) {
      const glm::vec3 feet = vec3Of(e["pes"], glm::vec3(0.0f));
      physics->setCharacterFeet(c->id, feet);
      physics->setCharacterVelocity(c->id, vel);
      registry.get<Transform>(ent).position = feet + glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f);
    } else if (Vehicle* v = registry.try_get<Vehicle>(ent); v && v->id != ~0u) {
      const uint32_t b = physics->vehicleBody(v->id);
      Transform& t = registry.get<Transform>(ent);
      t.position = vec3Of(e.value("posicao", json()), t.position);
      t.rotation = rot;
      physics->setPose(b, t.position, t.rotation);
      physics->setLinearVelocity(b, vel);
      physics->setAngularVelocity(b, ang);
    } else if (RigidBody* rb = registry.try_get<RigidBody>(ent); rb && rb->bodyId != ~0u) {
      Transform& t = registry.get<Transform>(ent);
      t.position = vec3Of(e.value("posicao", json()), t.position);
      t.rotation = rot;
      physics->setPose(rb->bodyId, t.position, t.rotation);
      if (rb->desc.motion == Motion::Dynamic) {
        physics->setLinearVelocity(rb->bodyId, vel);
        physics->setAngularVelocity(rb->bodyId, ang);
      }
      if (const Renderable* r = registry.try_get<Renderable>(ent); r && scene && registry.all_of<Shape>(ent))
        scene->setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(ent)));
    } else {
      continue;
    }
    ++applied;
  }
  time = doc.value("tempo", time);
  dataJson = doc.contains("dados") ? doc["dados"].dump() : std::string();
  LOG_INFO("Jogo carregado de %s (%zu entidades)", path.c_str(), applied);
  return true;
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
      if (c->remoteSlot >= 0) continue;  // vagas do multijogador: saem do "multijogador" da cena
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
      if (c->visual.enabled) {
        const CharacterVisual& v = c->visual;
        json vj = {{"modelo", v.path}, {"escala", v.scale}, {"desvio", toJson(v.offset)}, {"girar_graus", v.yawDeg},
                   {"velocidade_andar", v.walkRef}, {"velocidade_correr", v.runRef}, {"transicao", v.fade},
                   {"ik_pes", v.ik}};
        if (!v.idle.empty()) vj["parado"] = v.idle;
        if (!v.walk.empty()) vj["andar"] = v.walk;
        if (!v.run.empty()) vj["correr"] = v.run;
        json legs = json::array();
        for (const auto& l : v.legs) legs.push_back({l[0], l[1], l[2]});
        if (!legs.empty()) vj["pernas"] = legs;
        pj["visual"] = vj;
      }
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
    if (const ParticleSource* ps = registry.try_get<ParticleSource>(ent))
      e["particulas"] = {{"tipo", ps->type}, {"raio", ps->radius}, {"escala", ps->scale}, {"quantidade", ps->count},
                         {"desvio", toJson(ps->offset)}, {"ligado", ps->enabled}};
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
void setLanguageOverride(const std::string& code) { gLanguageOverride = code; }

void World::loadLanguages() {
  texts_.clear();
  languageNames_.clear();
  std::vector<std::filesystem::path> dirs = {"idiomas"};
  const std::filesystem::path sceneDir = std::filesystem::path(scenePath).parent_path();
  dirs.push_back(sceneDir / "idiomas");
  dirs.push_back(sceneDir.parent_path() / "idiomas");
  if (const std::filesystem::path exe = core::executableDir(); !exe.empty()) {  // pacote; build ao lado do motor
    dirs.push_back(exe / "idiomas");
    dirs.push_back(exe.parent_path() / "idiomas");
    dirs.push_back(exe.parent_path().parent_path() / "idiomas");  // build/Release no Windows
  }
  for (const auto& dir : dirs) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) continue;
    for (const auto& it : std::filesystem::directory_iterator(dir, ec)) {
      if (it.path().extension() != ".json") continue;
      const std::string code = it.path().stem().string();
      if (texts_.count(code)) continue;  // a primeira pasta vence
      std::ifstream f(it.path());
      try {
        const json doc = json::parse(f, nullptr, true, true);
        auto& table = texts_[code];
        for (auto kv = doc.begin(); kv != doc.end(); ++kv)
          if (kv.value().is_string()) table[kv.key()] = kv.value().get<std::string>();
        languageNames_[code] = table.count("_nome") ? table["_nome"] : code;
      } catch (const json::exception& e) {
        LOG_ERROR("Idioma %s: JSON inválido (%s)", it.path().string().c_str(), e.what());
      }
    }
  }
}

std::string World::translate(const std::string& key) const {
  for (const std::string& code : {language, std::string("pt")}) {
    auto t = texts_.find(code);
    if (t == texts_.end()) continue;
    auto k = t->second.find(key);
    if (k != t->second.end()) return k->second;
  }
  return key;
}

bool World::setLanguage(const std::string& code, bool remember) {
  if (!texts_.count(code)) return false;
  language = code;
  if (remember) {
    std::error_code ec;
    std::filesystem::create_directories("saves", ec);
    std::ofstream("saves/idioma.txt") << code;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> World::languages() const {
  std::vector<std::pair<std::string, std::string>> out(languageNames_.begin(), languageNames_.end());
  std::sort(out.begin(), out.end());
  return out;
}

float World::terrainHeight(float x, float z) const {
  if (!terrain.enabled) return 0.0f;
  const Terrain& T = terrain;
  if (!T.map.empty()) {  // mapa de altura cobrindo o lado do terreno, bilinear
    const float u = std::clamp((x - T.center.x) / T.size + 0.5f, 0.0f, 1.0f) * float(T.mapW - 1);
    const float v = std::clamp((z - T.center.y) / T.size + 0.5f, 0.0f, 1.0f) * float(T.mapH - 1);
    const int i = std::min(int(u), T.mapW - 2 < 0 ? 0 : T.mapW - 2), j = std::min(int(v), T.mapH - 2 < 0 ? 0 : T.mapH - 2);
    const int i1 = std::min(i + 1, T.mapW - 1), j1 = std::min(j + 1, T.mapH - 1);
    const float fu = u - float(i), fv = v - float(j);
    auto at = [&](int a, int b) { return T.map[size_t(b) * size_t(T.mapW) + size_t(a)]; };
    const float hgt = (at(i, j) * (1 - fu) + at(i1, j) * fu) * (1 - fv) + (at(i, j1) * (1 - fu) + at(i1, j1) * fu) * fv;
    return T.base + T.height * hgt;
  }
  // Ruído fractal: colinas grandes (escala em metros) com detalhe por oitava.
  // Relativo ao centro: o mesmo terreno em qualquer lugar do mundo.
  return T.base + T.height * scene::fbm((x - T.center.x) / T.noiseScale + T.seed, (z - T.center.y) / T.noiseScale - T.seed,
                                        T.octaves);
}

entt::entity World::entityOfBody(uint32_t body) const {
  if (body == ~0u) return entt::null;
  for (auto ent : registry.view<TerrainTile>())
    if (registry.get<TerrainTile>(ent).bodyId == body) return ent;
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

entt::entity World::slotEntity(int slot) const {
  if (slot < 0) return entt::null;
  for (auto ent : registry.view<Character, Transform>())
    if (registry.get<Character>(ent).remoteSlot == slot) return ent;
  return entt::null;
}

void World::activateSlot(int slot, bool on) {
  if (slot < 0 || size_t(slot) >= slotActive.size()) return;
  slotActive[size_t(slot)] = on;
  remoteInputs[size_t(slot)] = PlayerInput{};
  const entt::entity e = slotEntity(slot);
  if (e == entt::null) return;
  Character& c = registry.get<Character>(e);
  if (c.id == ~0u) return;
  const glm::vec3 feet = on ? slotSpawn[size_t(slot)] : glm::vec3(0.0f, -1000.0f - 10.0f * float(slot), 0.0f);
  physics->setCharacterFeet(c.id, feet);
  registry.get<Transform>(e).position = feet + glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
  LOG_INFO("Multijogador: vaga %d %s", slot, on ? "ocupada" : "livre");
}

entt::entity World::findByName(const std::string& name) const {
  for (auto e : registry.view<Name, Transform>())
    if (registry.get<Name>(e).value == name) return e;
  return entt::null;
}

bool World::sees(const Character& c, entt::entity other) const {
  if (c.id == ~0u || !registry.valid(other) || !registry.all_of<Transform>(other)) return false;
  const glm::vec3 eye = physics->characterFeet(c.id) + glm::vec3(0.0f, 0.85f * c.desc.height, 0.0f);
  glm::vec3 target = registry.get<Transform>(other).position;  // centro (personagem: meio do corpo)
  float radius = 0.3f;
  if (const Character* oc = registry.try_get<Character>(other)) {
    target.y += 0.25f * oc->desc.height;  // um pouco acima do meio: a cabeça/ombros
    radius = oc->desc.radius;
  }
  const glm::vec3 to = target - eye;
  const float dist = glm::length(to);
  if (dist > c.aiSight) return false;
  if (dist < 1e-3f) return true;
  const glm::vec3 dir = to / dist;
  const glm::vec2 flat(dir.x, dir.z), face(c.aiFacing.x, c.aiFacing.z);
  const bool hears = glm::length(glm::vec2(to.x, to.z)) < c.aiHearing;  // bem perto: percebe mesmo de costas
  if (!hears && glm::length(flat) > 1e-3f && glm::length(face) > 1e-3f &&
      glm::dot(glm::normalize(flat), glm::normalize(face)) < std::cos(glm::radians(0.5f * c.aiFovDeg)))
    return false;
  float hit = 0.0f;  // linha de visada: nada entre os olhos e o alvo (o próprio alvo pode ser o que o raio acerta)
  if (physics->castRayIgnoringCharacter(eye, dir, dist, c.id, &hit) && hit < dist - radius - 0.15f) return false;
  return true;
}

bool World::explode(glm::vec3 position, float scale) {
  if (explosionSlots.empty() || !scene) return false;
  scene->triggerExplosion(explosionSlots[nextExplosion++ % explosionSlots.size()], position, scale);
  return true;
}

bool World::playAction(entt::entity e, const std::string& clip, float fade, bool loop) {
  Character* c = registry.try_get<Character>(e);
  if (!c || !c->visual.enabled || c->visual.model < 0 || !scene) return false;
  const int a = scene->models[size_t(c->visual.model)].findAnimation(clip);
  if (a < 0) return false;
  CharacterVisual& v = c->visual;
  if (v.action != a) {
    v.action = a;
    v.actionTime = 0.0f;
    v.actionWeight = 0.0f;
  }
  v.actionOn = true;
  v.actionLoop = loop;
  v.actionFade = std::max(fade, 0.0f);
  return true;
}

void World::stopAction(entt::entity e, float fade) {
  if (Character* c = registry.try_get<Character>(e); c && c->visual.action >= 0) {
    c->visual.actionOn = false;
    c->visual.actionFade = std::max(fade, 0.0f);
  }
}

void World::partitionOpenWorld() {
  openWorld.cells.clear();
  std::map<std::pair<int, int>, size_t> index;
  const float cs = openWorld.cellSize;
  openWorld.entities = 0;
  for (auto ent : registry.view<Transform, Renderable>()) {
    if (registry.any_of<Character, Vehicle, SystemEntity, AlwaysLoaded, ParticleSource>(ent)) continue;
    if (const RigidBody* b = registry.try_get<RigidBody>(ent); b && b->desc.motion == Motion::Dynamic) continue;
    if (const Shape* sh = registry.try_get<Shape>(ent)) {  // maior que uma célula (chão, terreno): sempre
      const float extent = sh->type == ShapeType::Box ? 2.0f * std::max(sh->halfExtents.x, sh->halfExtents.z)
                                                      : 2.0f * sh->radius;
      if (extent > cs) continue;
    }
    const glm::vec3 p = registry.get<Transform>(ent).position;
    const std::pair<int, int> key{int(std::floor(p.x / cs)), int(std::floor(p.z / cs))};
    auto it = index.find(key);
    if (it == index.end()) {
      it = index.emplace(key, openWorld.cells.size()).first;
      OpenWorldCell c;
      c.center = glm::vec2((float(key.first) + 0.5f) * cs, (float(key.second) + 0.5f) * cs);
      openWorld.cells.push_back(c);
    }
    openWorld.cells[it->second].members.push_back(ent);
    ++openWorld.entities;
  }
  openWorld.loaded = uint32_t(openWorld.cells.size());
  LOG_INFO("Mundo aberto: %u entidades em %zu células de %.0f m (carrega a %.0f m, descarrega a %.0f m)",
           openWorld.entities, openWorld.cells.size(), cs, openWorld.loadDistance, openWorld.unloadDistance);
}

void World::setEntityDormant(scene::Scene& scene, entt::entity e, bool dormant) {
  if (!registry.valid(e) || registry.all_of<Dormant>(e) == dormant) return;
  if (dormant) registry.emplace<Dormant>(e);
  else registry.remove<Dormant>(e);
  if (const Renderable* r = registry.try_get<Renderable>(e)) scene.setObjectHidden(r->sceneObject, dormant);
  if (const RigidBody* b = registry.try_get<RigidBody>(e); b && b->bodyId != ~0u) physics->setBodyInWorld(b->bodyId, !dormant);
  if (const TerrainTile* tt = registry.try_get<TerrainTile>(e)) {  // de longe: a malha simples, sem física
    if (tt->farObject != ~0u) scene.setObjectHidden(tt->farObject, !dormant);
    if (tt->bodyId != ~0u) physics->setBodyInWorld(tt->bodyId, !dormant);
  }
  if (const SoundSource* snd = registry.try_get<SoundSource>(e); snd && snd->handle != ~0u && audio)
    audio->setPosition(snd->handle, dormant ? glm::vec3(0.0f, -1.0e6f, 0.0f) : registry.get<Transform>(e).position);
}

void World::updateOpenWorld(scene::Scene& scene) {
  if (!openWorld.enabled || openWorld.cells.empty()) return;
  glm::vec3 focus;
  if (!playerFeet(focus)) {
    if (!hasListener_) return;  // nem jogador nem câmera ainda: fica tudo carregado
    focus = listenerPos_;
  }
  const float half = 0.5f * openWorld.cellSize;
  uint32_t loaded = 0;
  for (OpenWorldCell& c : openWorld.cells) {
    const glm::vec2 d = glm::max(glm::abs(glm::vec2(focus.x, focus.z) - c.center) - half, glm::vec2(0.0f));
    const float dist = glm::length(d);  // até a borda do quadrado da célula
    const bool want = c.loaded ? dist <= openWorld.unloadDistance : dist < openWorld.loadDistance;
    if (want != c.loaded) {
      c.loaded = want;
      for (entt::entity e : c.members) setEntityDormant(scene, e, !want);
    }
    loaded += c.loaded ? 1 : 0;
  }
  openWorld.loaded = loaded;
}

bool World::startRagdoll(entt::entity e, glm::vec3 impulse) {
  Character* c = registry.try_get<Character>(e);
  if (!c || c->id == ~0u || !c->visual.enabled || c->visual.model < 0 || c->ragdoll.active || !scene || netClient)
    return false;
  scene::Model& m = scene->models[size_t(c->visual.model)];
  const size_t n = m.nodes.size();
  if (m.nodeWorld.size() != n || m.skins.empty()) return false;
  std::vector<uint8_t> joint(n, 0);
  for (const scene::Skin& sk : m.skins)
    for (uint32_t j : sk.joints)
      if (j < n) joint[j] = 1;
  std::vector<std::vector<uint32_t>> kids(n);
  for (size_t i = 0; i < n; ++i)
    if (joint[i] && m.nodes[i].parent >= 0 && joint[size_t(m.nodes[i].parent)]) kids[size_t(m.nodes[i].parent)].push_back(uint32_t(i));
  auto at = [&](size_t i) { return glm::vec3(m.nodeWorld[i][3]); };
  // Bacia: descendo da raiz do esqueleto, a primeira junta que se divide (pernas e coluna). Acima dela ficam ossos de
  // controle (raiz no chão), que não são corpo de verdade.
  int pelvis = -1;
  for (size_t i = 0; i < n && pelvis < 0; ++i) {
    if (!joint[i] || (m.nodes[i].parent >= 0 && joint[size_t(m.nodes[i].parent)])) continue;
    size_t j = i;
    while (kids[j].size() == 1) j = kids[j][0];
    if (kids[j].size() >= 2) pelvis = int(j);
  }
  if (pelvis < 0) return false;
  const float h = c->desc.height, minLen = 0.04f * h;
  std::vector<RagdollPart> parts;
  std::vector<uint32_t> nodes;
  std::vector<int> partOf(n, -1);
  std::vector<uint32_t> stack{uint32_t(pelvis)};  // pais antes dos filhos
  while (!stack.empty()) {
    const uint32_t i = stack.back();
    stack.pop_back();
    for (auto it = kids[i].rbegin(); it != kids[i].rend(); ++it) stack.push_back(*it);
    const glm::vec3 a = at(i);
    glm::vec3 b = a;
    float radius = 0.0f;
    if (!kids[i].empty()) {
      glm::vec3 cen(0.0f);
      float far = 0.0f;
      for (uint32_t k : kids[i]) {
        cen += at(k);
        far = std::max(far, glm::length(at(k) - a));
      }
      cen /= float(kids[i].size());
      if (far < minLen) continue;
      if (glm::length(cen - a) > 0.3f * far) {
        b = cen;
        radius = glm::clamp(0.22f * glm::length(b - a), 0.015f * h, 0.1f * h);
      } else {  // filhos para lados opostos (bacia humana): uma esfera
        radius = glm::clamp(0.35f * far, 0.015f * h, 0.12f * h);
      }
    } else {  // ponta (cabeça, mão, pé): continua na direção do osso, 60% do comprimento do pai
      if (m.nodes[i].parent < 0) continue;
      const glm::vec3 d = a - at(size_t(m.nodes[i].parent));
      if (glm::length(d) < minLen) continue;
      b = a + 0.6f * d;
      radius = glm::clamp(0.22f * glm::length(b - a), 0.015f * h, 0.1f * h);
    }
    int parent = -1;
    for (int p = m.nodes[i].parent; p >= 0 && parent < 0; p = m.nodes[size_t(p)].parent) parent = partOf[size_t(p)];
    if (parent < 0 && !parts.empty()) continue;  // fora da árvore da bacia
    RagdollPart part;
    part.parent = parent;
    part.a = a;
    part.b = b;
    part.radius = radius;
    partOf[i] = int(parts.size());
    parts.push_back(part);
    nodes.push_back(i);
  }
  if (parts.empty()) return false;
  // Massa pelo volume de cada parte, somando a do personagem.
  float volume = 0.0f;
  std::vector<float> vol(parts.size());
  for (size_t k = 0; k < parts.size(); ++k) {
    const float r = parts[k].radius, len = glm::length(parts[k].b - parts[k].a);
    vol[k] = glm::pi<float>() * r * r * std::max(len - 2.0f * r, 0.0f) + 4.0f / 3.0f * glm::pi<float>() * r * r * r;
    volume += vol[k];
  }
  for (size_t k = 0; k < parts.size(); ++k) parts[k].mass = c->desc.mass * vol[k] / std::max(volume, 1e-9f);
  const glm::vec3 velocity = physics->characterVelocity(c->id) + impulse / std::max(c->desc.mass, 1e-3f);
  std::vector<uint32_t> bodies = physics->addRagdoll(parts, velocity);
  if (bodies.empty()) return false;
  Character::Ragdoll& r = c->ragdoll;
  r.active = true;
  r.bodies = bodies;
  r.nodes = nodes;
  r.parents.clear();
  for (const RagdollPart& part : parts) r.parents.push_back(part.parent);
  r.offset.resize(bodies.size());
  m.driven.assign(n, -1);
  m.drivenWorld.resize(bodies.size());
  r.fallRotation.resize(bodies.size());
  r.strength = 0.0f;
  r.followAnimation = false;
  r.motorsOn = false;
  for (size_t k = 0; k < bodies.size(); ++k) {
    glm::vec3 p;
    glm::quat q;
    physics->pose(bodies[k], p, q);
    r.fallRotation[k] = q;
    r.offset[k] = glm::inverse(glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q)) * m.nodeWorld[nodes[k]];
    m.driven[nodes[k]] = int(k);
    m.drivenWorld[k] = m.nodeWorld[nodes[k]];
  }
  r.rootBasis = m.root;
  r.rootBasis[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
  r.rootToPelvis = at(nodes[0]) - glm::vec3(m.root[3]);
  physics->setCharacterFeet(c->id, glm::vec3(0.0f, -3000.0f - 10.0f * float(entt::to_integral(e) & 1023u), 0.0f));
  return true;
}

bool World::stopRagdoll(entt::entity e) {
  Character* c = registry.try_get<Character>(e);
  if (!c || !c->ragdoll.active || !scene) return false;
  scene::Model& m = scene->models[size_t(c->visual.model)];
  glm::vec3 pelvis(m.drivenWorld[0][3]);
  // De pé olhando para onde estava o tronco: o centro do maior ramo da bacia (coluna, cabeça e braços; não a cauda
  // nem uma perna).
  const std::vector<int>& par = c->ragdoll.parents;
  std::vector<int> branch(par.size(), -1), count(par.size(), 0);
  for (size_t k = 1; k < par.size(); ++k) {
    int b = int(k);
    while (b >= 0 && par[size_t(b)] != 0) b = par[size_t(b)];
    branch[k] = b;
    if (b >= 0) ++count[size_t(b)];
  }
  const int big = int(std::max_element(count.begin(), count.end()) - count.begin());
  glm::vec3 along(0.0f);
  for (size_t k = 1; k < par.size() && k < m.drivenWorld.size(); ++k)
    if (branch[k] == big) along += glm::vec3(m.drivenWorld[k][3]) - pelvis;
  physics->removeRagdoll(c->ragdoll.bodies);
  // A pose caída fica parada em drivenWorld e se mistura com a animação até sumir (sem salto).
  c->ragdoll = Character::Ragdoll{};
  c->ragdoll.blendOut = World::kRagdollBlendOut;
  m.drivenWeight = 1.0f;
  if (glm::length(glm::vec2(along.x, along.z)) > 1e-3f) c->visual.yaw = std::atan2(along.x, along.z);
  // De pé onde a bacia está: os pés no chão abaixo dela (ou na altura dela, sem chão).
  float dist = 0.0f;
  glm::vec3 feet = pelvis;
  if (physics->castRay(pelvis + glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f), 2.0f * c->desc.height, &dist) != ~0u)
    feet.y = pelvis.y + 0.5f * c->desc.height - dist + 0.02f;
  physics->setCharacterFeet(c->id, feet);
  return true;
}

bool World::bonePosition(entt::entity e, const std::string& bone, glm::vec3& out) const {
  const Character* c = registry.try_get<Character>(e);
  if (!c || !c->visual.enabled || c->visual.model < 0 || !scene) return false;
  const scene::Model& m = scene->models[size_t(c->visual.model)];
  const int n = m.findNode(bone);
  if (n < 0 || size_t(n) >= m.nodeWorld.size()) return false;
  out = glm::vec3(m.nodeWorld[size_t(n)][3]);
  return true;
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
  // Personagem: a posição é a do centro da cápsula (como no arquivo de cena); teletransporta os pés.
  if (const Character* c = registry.try_get<Character>(e); c && c->id != ~0u)
    physics->setCharacterFeet(c->id, position - glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f));
  if (const Renderable* r = registry.try_get<Renderable>(e); r && registry.all_of<Shape>(e))
    scene.setTransform(r->sceneObject, drawMatrix(t, registry.get<Shape>(e)));
  // Modelos glTF e personagens: mover no editor fica para depois (o modelo tem vários objetos e colisão por malha).
}

bool World::playerFeet(glm::vec3& feet) const {
  if (netClient) {  // a câmera do cliente segue a vaga dele (pose replicada)
    const entt::entity e = slotEntity(localSlot);
    if (e == entt::null) return false;
    const Character& c = registry.get<Character>(e);
    feet = registry.get<Transform>(e).position - glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
    return true;
  }
  for (auto ent : registry.view<Character>()) {
    const Character& c = registry.get<Character>(ent);
    if (c.player && c.id != ~0u) {
      feet = physics->characterFeet(c.id);
      return true;
    }
  }
  return false;
}

// Script visual: arquivo .json (recarregado quando muda) ou o grafo direto na entidade. O Lua gerado vai para uma
// pasta temporária com nome fixo pelo arquivo de origem (a recarga a quente do Lua pega a mudança).
std::string World::compileVisualSource(const nlohmann::json& src, const std::string& sceneDir) {
  nlohmann::json graph;
  std::string from = "(na cena)";
  if (src.is_string()) {
    from = src.get<std::string>();
    if (!std::ifstream(from) && std::ifstream(sceneDir + from)) from = sceneDir + from;
    std::ifstream f(from);
    try {
      graph = nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& ex) {
      LOG_ERROR("Script visual %s: JSON inválido (%s)", from.c_str(), ex.what());
      return {};
    }
  } else {
    graph = src;
  }
  std::string error;
  const std::string lua = compileVisualScript(graph, error);
  if (lua.empty()) {
    LOG_ERROR("Script visual %s: %s", from.c_str(), error.c_str());
    return {};
  }
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "legacy_script_visual";
  std::filesystem::create_directories(dir, ec);
  const size_t key = std::hash<std::string>{}(src.is_string() ? std::filesystem::absolute(from, ec).string() : graph.dump());
  const std::filesystem::path out = dir / (std::to_string(key) + ".lua");
  std::ofstream(out, std::ios::binary) << lua;
  if (src.is_string()) {
    VisualSource vs;
    vs.source = from;
    vs.lua = out.string();
    vs.stamp = std::filesystem::last_write_time(from, ec);
    visualSources_.push_back(vs);
  }
  return out.string();
}

void World::refreshVisualScripts() {
  for (VisualSource& vs : visualSources_) {
    std::error_code ec;
    const auto now = std::filesystem::last_write_time(vs.source, ec);
    if (ec || now == vs.stamp) continue;
    vs.stamp = now;
    std::ifstream f(vs.source);
    std::string error, lua;
    try {
      lua = compileVisualScript(nlohmann::json::parse(f), error);
    } catch (const nlohmann::json::exception& ex) {
      error = ex.what();
    }
    if (lua.empty()) {
      LOG_ERROR("Script visual %s: %s (fica a versão anterior)", vs.source.c_str(), error.c_str());
      continue;
    }
    std::ofstream(vs.lua, std::ios::binary) << lua;
    LOG_INFO("Script visual %s mudou: Lua gerado de novo", vs.source.c_str());
  }
}

// Malha de um ladrilho do terreno com centro (cx, cz): posições relativas ao centro (precisão), altura absoluta,
// normais e UVs em metros do mundo (sem emenda entre ladrilhos) e uma saia vertical na borda que esconde as frestas entre
// ladrilhos de resoluções diferentes.
scene::MeshData World::terrainTileMesh(float cx, float cz, uint32_t r, float skirt) const {
  const Terrain& T = terrain;
  scene::MeshData m;
  const float step = T.tile / float(r), half = 0.5f * T.tile, e = 0.5f * step;
  auto vert = [&](float lx, float lz, float dy) {
    const float x = cx + lx, z = cz + lz;
    const glm::vec3 nrm = glm::normalize(glm::vec3(terrainHeight(x - e, z) - terrainHeight(x + e, z), 2.0f * e,
                                                   terrainHeight(x, z - e) - terrainHeight(x, z + e)));
    m.vertices.push_back({{lx, terrainHeight(x, z) - dy, lz}, nrm, {x, z}, {}});
  };
  for (uint32_t j = 0; j <= r; ++j)
    for (uint32_t i = 0; i <= r; ++i) vert(-half + float(i) * step, -half + float(j) * step, 0.0f);
  for (uint32_t j = 0; j < r; ++j)
    for (uint32_t i = 0; i < r; ++i) {
      const uint32_t a = j * (r + 1) + i, b = a + r + 1;
      m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
  // Saia: a borda (no sentido do contorno) e uma cópia mais baixa; os dois lados, para valer de qualquer ângulo.
  std::vector<uint32_t> ring;
  for (uint32_t i = 0; i < r; ++i) ring.push_back(i);
  for (uint32_t j = 0; j < r; ++j) ring.push_back(j * (r + 1) + r);
  for (uint32_t i = r; i > 0; --i) ring.push_back(r * (r + 1) + i);
  for (uint32_t j = r; j > 0; --j) ring.push_back(j * (r + 1));
  const uint32_t base = uint32_t(m.vertices.size());
  for (uint32_t k : ring) {
    scene::Vertex v = m.vertices[k];
    v.position.y -= skirt;
    m.vertices.push_back(v);
  }
  for (size_t k = 0; k < ring.size(); ++k) {
    const uint32_t a = ring[k], b = ring[(k + 1) % ring.size()];
    const uint32_t la = base + uint32_t(k), lb = base + uint32_t((k + 1) % ring.size());
    m.indices.insert(m.indices.end(), {a, la, b, b, la, lb, a, b, la, b, lb, la});
  }
  scene::computeTangents(m);
  return m;
}

// Pincel do terreno (editor e Lua). Na primeira pincelada, a altura (ruído ou mapa) vira uma grade editável com um ponto
// por vértice dos ladrilhos de perto; depois, os ladrilhos tocados são refeitos (malha de perto e de longe, colisão).
bool World::terrainBrush(scene::Scene& scene, glm::vec2 at, float radius, float strength, const std::string& mode) {
  Terrain& T = terrain;
  if (!T.enabled || radius <= 0.0f) return false;
  const int n = std::max(1, int(std::ceil(T.size / T.tile)));
  const float span = float(n) * T.tile;
  if (!T.editable) {  // amostra a altura atual numa grade alinhada aos vértices dos ladrilhos
    const int w = n * int(T.res) + 1;
    std::vector<float> grid(size_t(w) * size_t(w));
    const float step = span / float(w - 1);
    for (int j = 0; j < w; ++j)
      for (int i = 0; i < w; ++i)
        grid[size_t(j) * size_t(w) + size_t(i)] =
            terrainHeight(T.center.x - 0.5f * span + float(i) * step, T.center.y - 0.5f * span + float(j) * step);
    T.map = std::move(grid);
    T.mapW = T.mapH = w;
    T.size = span;
    T.base = 0.0f;
    T.height = 1.0f;
    T.editable = true;
  }
  const float step = T.size / float(T.mapW - 1);
  const glm::vec2 origin = T.center - glm::vec2(0.5f * T.size);
  const int i0 = std::max(0, int(std::floor((at.x - radius - origin.x) / step))),
            i1 = std::min(T.mapW - 1, int(std::ceil((at.x + radius - origin.x) / step)));
  const int j0 = std::max(0, int(std::floor((at.y - radius - origin.y) / step))),
            j1 = std::min(T.mapH - 1, int(std::ceil((at.y + radius - origin.y) / step)));
  if (i0 > i1 || j0 > j1) return false;
  std::vector<float> before = T.map;  // suavizar lê os vizinhos de antes
  auto at0 = [&](int i, int j) { return before[size_t(std::clamp(j, 0, T.mapH - 1)) * size_t(T.mapW) + size_t(std::clamp(i, 0, T.mapW - 1))]; };
  for (int j = j0; j <= j1; ++j)
    for (int i = i0; i <= i1; ++i) {
      const glm::vec2 p = origin + glm::vec2(float(i), float(j)) * step;
      const float d = glm::length(p - at) / radius;
      if (d >= 1.0f) continue;
      const float w = 0.5f + 0.5f * std::cos(3.14159265f * d);  // queda suave até a borda do pincel
      float& h = T.map[size_t(j) * size_t(T.mapW) + size_t(i)];
      if (mode == "subir") h += strength * w;
      else if (mode == "descer") h -= strength * w;
      else if (mode == "aplanar") h += (strength - h) * w;  // strength = altura alvo
      else if (mode == "suavizar") {
        const float avg = (at0(i - 1, j) + at0(i + 1, j) + at0(i, j - 1) + at0(i, j + 1) + at0(i, j)) / 5.0f;
        h += (avg - h) * std::clamp(strength, 0.0f, 1.0f) * w;
      } else {
        LOG_WARN("Pincel do terreno: modo '%s' desconhecido (subir, descer, aplanar, suavizar)", mode.c_str());
        return false;
      }
    }
  // Ladrilhos tocados (a borda do pincel + a margem da normal): malha nova, colisão nova.
  uint32_t rebuilt = 0;
  for (auto ent : registry.view<TerrainTile, Renderable, Transform>()) {
    TerrainTile& tt = registry.get<TerrainTile>(ent);
    const glm::vec2 lo = tt.center - glm::vec2(0.5f * T.tile + step), hi = tt.center + glm::vec2(0.5f * T.tile + step);
    const glm::vec2 c = glm::clamp(at, lo, hi);
    if (glm::length(c - at) > radius) continue;
    scene::MeshData nearMesh = terrainTileMesh(tt.center.x, tt.center.y, T.res, 0.02f * T.tile);
    const uint32_t obj = registry.get<Renderable>(ent).sceneObject;
    std::vector<glm::vec3> verts;
    verts.reserve(nearMesh.vertices.size());
    for (const scene::Vertex& v : nearMesh.vertices) verts.push_back(v.position + glm::vec3(tt.center.x, 0.0f, tt.center.y));
    if (tt.bodyId != ~0u) {
      const bool inWorld = physics->bodyInWorld(tt.bodyId);
      physics->removeBody(tt.bodyId);
      tt.bodyId = physics->addStaticMesh(verts, nearMesh.indices, tt.friction);
      if (!inWorld) physics->setBodyInWorld(tt.bodyId, false);
    }
    scene.replaceMesh(scene.objects[obj].mesh, nearMesh);
    if (tt.farObject != ~0u)
      scene.replaceMesh(scene.objects[tt.farObject].mesh, terrainTileMesh(tt.center.x, tt.center.y, T.farRes, 0.08f * T.tile));
    registry.get<Transform>(ent).position.y = terrainHeight(tt.center.x, tt.center.y);
    ++rebuilt;
  }
  if (std::getenv("LEGACY_DEBUG_TERRENO")) LOG_INFO("Pincel %s em (%.1f, %.1f): %u ladrilhos refeitos", mode.c_str(), at.x, at.y, rebuilt);
  return rebuilt > 0;
}

// Grava a grade de alturas (".altura": "LGAL", largura, altura, lado em m, centro x, z e as alturas em float) para
// usar como "mapa" do terreno.
bool World::saveTerrainMap(const std::string& path) const {
  const Terrain& T = terrain;
  if (!T.enabled || T.map.empty()) return false;
  std::ofstream f(path, std::ios::binary);
  const float hdr[5] = {float(T.mapW), float(T.mapH), T.size, T.center.x, T.center.y};
  f.write("LGAL", 4);
  f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
  if (T.editable) {
    f.write(reinterpret_cast<const char*>(T.map.data()), std::streamsize(T.map.size() * sizeof(float)));
  } else {  // mapa PNG (0..1): grava em metros
    for (float v : T.map) {
      const float m = T.base + T.height * v;
      f.write(reinterpret_cast<const char*>(&m), sizeof(m));
    }
  }
  return bool(f);
}

// Rotação no mundo que cada parte do ragdoll teria na pose pura da animação (corpo = nó × inverso do deslocamento).
std::vector<glm::quat> World::ragdollTargets(const scene::Model& m, const Character::Ragdoll& r) const {
  if (!r.followAnimation || m.animPose.size() != m.nodes.size()) return r.fallRotation;  // a pose de quando caiu
  std::vector<glm::quat> out(r.bodies.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
  for (size_t k = 0; k < r.bodies.size(); ++k) {
    const glm::mat4 body = m.root * m.animPose[r.nodes[k]] * glm::inverse(r.offset[k]);
    const glm::vec3 x = glm::normalize(glm::vec3(body[0])), y = glm::normalize(glm::vec3(body[1]));
    const glm::vec3 z = glm::normalize(glm::cross(x, y));
    out[k] = glm::normalize(glm::quat_cast(glm::mat3(x, glm::cross(z, x), z)));
  }
  return out;
}

bool World::setRagdollStrength(entt::entity e, float strength, bool followAnimation) {
  Character* c = registry.try_get<Character>(e);
  if (!c || !c->ragdoll.active) return false;
  c->ragdoll.strength = glm::clamp(strength, 0.0f, 1.0f);
  c->ragdoll.followAnimation = followAnimation;
  if (scene && c->visual.model >= 0 && followAnimation) {
    scene::Model& m = scene->models[size_t(c->visual.model)];
    m.keepAnimPose = c->ragdoll.strength > 0.0f;
    if (m.keepAnimPose && m.animPose.size() != m.nodes.size()) scene->poseModel(c->visual.model);  // a pose alvo já
  }
  return true;
}

float World::ragdollDeviation(entt::entity e) const {
  const Character* c = registry.try_get<Character>(e);
  if (!c || !c->ragdoll.active || !scene || c->visual.model < 0) return -1.0f;
  scene::Model& m = scene->models[size_t(c->visual.model)];
  if (c->ragdoll.followAnimation && m.animPose.size() != m.nodes.size()) {
    m.keepAnimPose = true;
    scene->poseModel(c->visual.model);
  }
  const std::vector<glm::quat> target = ragdollTargets(m, c->ragdoll);
  float sum = 0.0f;
  int count = 0;
  for (size_t k = 0; k < c->ragdoll.bodies.size(); ++k) {
    const int p = c->ragdoll.parents[k];
    if (p < 0) continue;
    glm::vec3 pp, pc;
    glm::quat qp, qc;
    physics->pose(c->ragdoll.bodies[size_t(p)], pp, qp);
    physics->pose(c->ragdoll.bodies[k], pc, qc);
    const glm::quat now = glm::inverse(qp) * qc, want = glm::inverse(target[size_t(p)]) * target[k];
    const float d = std::abs(glm::dot(glm::normalize(now), glm::normalize(want)));
    sum += glm::degrees(2.0f * std::acos(std::min(d, 1.0f)));
    ++count;
  }
  return count ? sum / float(count) : -1.0f;
}

// Retargeting de um personagem: carrega o modelo de origem (escondido: só o esqueleto e as animações interessam) e
// assa no modelo do personagem cada clipe dele, pelo mapa de ossos ou pelos nomes (sem os prefixos).
void World::retargetCharacter(scene::Scene& scene, int model, const CharacterVisual& v) {
  int src = -1;
  if (!scene::loadGltf(scene, v.retargetFrom, glm::mat4(1.0f), &src) || src < 0) {
    LOG_WARN("Retargeting: não carreguei %s", v.retargetFrom.c_str());
    return;
  }
  scene::Model& s = scene.models[size_t(src)];
  for (const auto& [node, object] : s.rigidObjects) scene.setObjectHidden(object, true);
  for (uint32_t object : s.skinnedObjects) scene.setObjectHidden(object, true);
  s.animation = -1;
  scene::Model& d = scene.models[size_t(model)];
  std::vector<std::pair<std::string, std::string>> map = v.retargetMap;
  if (map.empty())
    for (const std::string& n : s.nodeNames) {
      if (n.rfind(v.retargetSrcPrefix, 0) != 0) continue;
      const std::string dn = v.retargetDstPrefix + n.substr(v.retargetSrcPrefix.size());
      if (d.findNode(dn) >= 0) map.push_back({n, dn});
    }
  uint32_t made = 0;
  for (size_t a = 0; a < s.animations.size(); ++a) {
    const std::string name = s.animations[a].name.empty() ? "clipe" + std::to_string(a) : s.animations[a].name;
    if (scene::retargetAnimation(d, s, int(a), map, name) >= 0) ++made;
  }
  LOG_INFO("Retargeting: %u animações de %s em %zu ossos casados", made, v.retargetFrom.c_str(), map.size());
}

}  // namespace game
