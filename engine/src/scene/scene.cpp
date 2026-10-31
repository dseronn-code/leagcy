#include "scene/scene.h"

#include <chrono>

#include "scene/texture_compress.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <map>

namespace scene {

namespace {
// Espelhado em shaders/scene.glsl (InstanceData).
struct InstanceGpu {  // == InstanceData em shaders/scene.glsl
  uint64_t vertices, indices;
  uint32_t material, pad0;
  uint64_t colors;  // cor por vértice (COLOR_0) ou 0
};
static_assert(sizeof(InstanceGpu) == 32);

// Espelhado em shaders/skinning.comp.
struct SkinVertex {
  glm::uvec4 joints;
  glm::vec4 weights;
};

glm::mat4 nodeMatrix(const ModelNode& n) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), n.translation) * glm::mat4_cast(n.rotation);
  return glm::scale(m, n.scale);
}
}  // namespace

int Model::findAnimation(const std::string& name) const {
  for (size_t i = 0; i < animations.size(); ++i)
    if (animations[i].name == name) return int(i);
  return -1;
}

void Scene::animateModel(Model& m, float dt) {
  m.time += dt * m.speed;
  std::vector<ModelNode> pose = m.nodes;
  if (m.animation >= 0 && size_t(m.animation) < m.animations.size()) {
    const Animation& a = m.animations[size_t(m.animation)];
    float t = a.duration > 0.0f ? std::fmod(m.time, a.duration) : 0.0f;
    if (t < 0.0f) t += a.duration;
    for (const AnimationChannel& ch : a.channels) {
      const AnimationSampler& s = a.samplers[ch.sampler];
      if (s.times.empty() || ch.node >= pose.size()) continue;
      size_t i1 = size_t(std::upper_bound(s.times.begin(), s.times.end(), t) - s.times.begin());
      size_t i0 = i1 == 0 ? 0 : i1 - 1;
      i1 = std::min(i1, s.times.size() - 1);
      float span = s.times[i1] - s.times[i0];
      float f = (s.step || span <= 0.0f) ? 0.0f : glm::clamp((t - s.times[i0]) / span, 0.0f, 1.0f);
      const glm::vec4 &v0 = s.values[i0], &v1 = s.values[i1];
      ModelNode& n = pose[ch.node];
      if (ch.path == AnimationChannel::kRotation) {
        n.rotation = glm::normalize(glm::slerp(glm::quat(v0.w, v0.x, v0.y, v0.z), glm::quat(v1.w, v1.x, v1.y, v1.z), f));
      } else {
        glm::vec3 v = glm::mix(glm::vec3(v0), glm::vec3(v1), f);
        (ch.path == AnimationChannel::kTranslation ? n.translation : n.scale) = v;
      }
    }
  }

  // Matrizes globais (pai antes do filho, com memoização).
  std::vector<glm::mat4> global(pose.size());
  std::vector<uint8_t> done(pose.size(), 0);
  std::function<const glm::mat4&(size_t)> resolve = [&](size_t i) -> const glm::mat4& {
    if (!done[i]) {
      glm::mat4 local = nodeMatrix(pose[i]);
      global[i] = pose[i].parent >= 0 ? resolve(size_t(pose[i].parent)) * local : local;
      done[i] = 1;
    }
    return global[i];
  };
  for (auto [node, object] : m.rigidObjects) setTransform(object, m.root * resolve(node));
  for (uint32_t object : m.skinnedObjects) setTransform(object, m.root);
  m.jointMatrices.resize(m.skins.size());
  for (size_t s = 0; s < m.skins.size(); ++s) {
    const Skin& skin = m.skins[s];
    m.jointMatrices[s].resize(skin.joints.size());
    for (size_t j = 0; j < skin.joints.size(); ++j) m.jointMatrices[s][j] = resolve(skin.joints[j]) * skin.inverseBind[j];
  }
}

void Scene::update(float dt) {
  time += dt;
  // Os movidos no frame anterior assentam (anterior = atual); os outros já estão assim.
  for (uint32_t i : movedLast_) objects[i].prevTransform = objects[i].transform;
  // Movidos fora de um update (pose inicial da animação, antes do primeiro frame): sem movimento a mostrar.
  for (uint32_t i : movedNow_) {
    movedMark_[i] = 0;
    objects[i].prevTransform = objects[i].transform;
  }
  movedNow_.clear();
  movedMark_.resize(objects.size(), 0);
  for (auto& a : animators_) a(*this, time, dt);
  for (auto& m : models) animateModel(m, dt);
  for (uint32_t i : crowdObjects_) {  // multidão: avança a pose (e reenvia o objeto: a pose mudou)
    Object& o = objects[i];
    const CrowdClip& c = crowdClips[o.crowdClip];
    const float t = std::fmod(time + o.animPhase, c.duration);
    o.prevAnimFrame = o.animFrame;
    o.animFrame = uint32_t(std::floor(t / c.duration * float(c.frames))) % c.frames;
    if (o.animFrame != o.prevAnimFrame || dt > 0.0f) setTransform(i, o.transform);
  }
  for (uint32_t i : movedNow_)  // luzes de malha acompanham o objeto
    if (i < meshLightSourceOf_.size() && meshLightSourceOf_[i] >= 0) writeMeshLights(meshLightSources_[size_t(meshLightSourceOf_[i])]);
  changed_ = movedLast_;
  changed_.insert(changed_.end(), movedNow_.begin(), movedNow_.end());
  for (uint32_t i : movedNow_) movedMark_[i] = 0;
  movedLast_.swap(movedNow_);
}

void Scene::setTransform(uint32_t object, const glm::mat4& transform) {
  if (object >= movedMark_.size()) movedMark_.resize(objects.size(), 0);
  if (!movedMark_[object]) {
    movedMark_[object] = 1;
    movedNow_.push_back(object);
  }
  objects[object].transform = transform;
}

rhi::TlasInstance Scene::tlasInstance(uint32_t i) const {
  const uint32_t flags = materials[objects[i].material].flags;
  const bool opaque = !(flags & kMaterialAlphaMask);
  // Máscaras (shaders/raytracing.glsl): 0x01 = faz sombra opaca, 0x02 = globo de lâmpada (sem sombra), 0x04 = vidro
  // (sombra colorida, pela transmitância).
  const uint8_t mask = (flags & kMaterialLightGeometry) ? 0x02 : (flags & kMaterialTransmissive) ? 0x04 : 0xFF;
  // Multidão: a BLAS da pose deste frame (cada pose assada tem a sua).
  const uint32_t mesh = objects[i].crowdClip != ~0u ? crowdClips[objects[i].crowdClip].firstMesh + objects[i].animFrame
                                                    : objects[i].mesh;
  return {&meshes[mesh].blas, objects[i].transform, i, opaque, mask};  // só chamado para objetos com malha na memória
}

// Objetos com malha fora da memória (streaming) ficam fora do TLAS: nem como instância inativa (o llvmpipe trava
// com uma mistura de ativas e inativas; outros drivers também podem tratar mal), nem pagando espaço na hierarquia.
void Scene::rebuildTlasLayout() {
  tlasSlot_.assign(objects.size(), -1);
  tlasCount_ = 0;
  for (uint32_t i = 0; i < objects.size(); ++i)
    if (meshes[objects[i].mesh].resident && !objects[i].hlodProxy) tlasSlot_[i] = int32_t(tlasCount_++);
  ++tlasLayout_;
}

namespace {
// Meshlets de uma malha rígida (com mesh shaders), num bloco só: [Meshlet × n][vértices][triângulos]. Preenche os
// contadores da malha; vazio sem mesh shaders ou para malha deformável.
std::vector<uint32_t> meshletBlob(const rhi::Context& ctx, GpuMesh& gm, const MeshData& data) {
  if (!ctx.meshShaders || data.clothWidth > 0 || data.hairStrands > 0 || !data.joints.empty() || data.indices.empty()) return {};
  const MeshletData md = buildMeshlets(data);
  std::vector<uint32_t> blob(md.meshlets.size() * (sizeof(Meshlet) / 4));
  std::memcpy(blob.data(), md.meshlets.data(), md.meshlets.size() * sizeof(Meshlet));
  gm.meshletCount = uint32_t(md.meshlets.size());
  gm.meshletVertexBase = uint32_t(blob.size());
  blob.insert(blob.end(), md.vertices.begin(), md.vertices.end());
  gm.meshletTriangleBase = uint32_t(blob.size());
  blob.insert(blob.end(), md.triangles.begin(), md.triangles.end());
  return blob;
}
VkDeviceSize uploadColors(rhi::Context& ctx, GpuMesh& gm, const MeshData& data) {
  if (data.colors.empty()) return 0;
  gm.colors = rhi::createBufferWithData(ctx, data.colors.data(), data.colors.size() * sizeof(glm::vec4),
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  return gm.colors.size;
}
VkDeviceSize uploadMeshlets(rhi::Context& ctx, GpuMesh& gm, const std::vector<uint32_t>& blob) {
  if (blob.empty()) return 0;
  gm.meshlets = rhi::createBufferWithData(ctx, blob.data(), blob.size() * sizeof(uint32_t),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
  return gm.meshlets.size;
}
}  // namespace

void Scene::buildHlod() {
  if (hlodCellSize <= 0.0f) return;
  // Grupos (célula, material). Fora: o que se deforma, anima ou depende do vertex shader (mar, cabelo, folhagem,
  // multidão), decals misturados, malhas de streaming e objetos maiores que a célula (o chão da cidade inteira).
  std::map<std::pair<std::pair<int, int>, uint32_t>, std::vector<uint32_t>> groups;
  std::map<std::pair<int, int>, int32_t> cellOf;
  const uint32_t skip = kMaterialWater | kMaterialHair | kMaterialFoliage | kMaterialBlend;
  const size_t count = objects.size();
  for (uint32_t i = 0; i < count; ++i) {
    const Object& o = objects[i];
    if (!o.hlodCandidate || o.crowdClip != ~0u || (materials[o.material].flags & skip)) continue;
    if (o.mesh >= cpuMeshes_.size() || (o.mesh < meshCellPending_.size() && meshCellPending_[o.mesh] >= 0)) continue;
    const MeshData& md = cpuMeshes_[o.mesh];
    if (!md.joints.empty() || md.clothWidth > 0 || md.indices.empty()) continue;
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const Vertex& v : md.vertices) {
      const glm::vec3 p = glm::vec3(o.transform * glm::vec4(v.position, 1.0f));
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
    if (glm::max(hi.x - lo.x, hi.z - lo.z) > hlodCellSize) continue;
    const glm::vec3 c = 0.5f * (lo + hi);
    const std::pair<int, int> key{int(std::floor(c.x / hlodCellSize)), int(std::floor(c.z / hlodCellSize))};
    auto it = cellOf.find(key);
    if (it == cellOf.end()) {
      it = cellOf.emplace(key, int32_t(hlodCells.size())).first;
      hlodCells.push_back({});
      hlodCells.back().distance = hlodDistance;
    }
    HlodCell& cell = hlodCells[size_t(it->second)];
    cell.lo = glm::min(cell.lo, lo);
    cell.hi = glm::max(cell.hi, hi);
    groups[{key, o.material}].push_back(i);
  }
  size_t proxies = 0, members = 0, trisIn = 0, trisOut = 0;
  for (const auto& [key, list] : groups) {
    if (list.size() < 2) continue;  // um objeto só: nada a fundir
    const int32_t cell = cellOf[key.first];
    const HlodCell& hc = hlodCells[size_t(cell)];
    const glm::vec3 center(0.5f * (hc.lo.x + hc.hi.x), hc.lo.y, 0.5f * (hc.lo.z + hc.hi.z));
    MeshData merged;
    bool colored = false;
    for (uint32_t i : list) colored = colored || !cpuMeshes_[objects[i].mesh].colors.empty();
    for (uint32_t i : list) {
      const Object& o = objects[i];
      const MeshData& md = cpuMeshes_[o.mesh];
      const glm::mat3 m3(o.transform), nm = glm::transpose(glm::inverse(m3));
      const bool mirrored = glm::determinant(m3) < 0.0f;
      const uint32_t base = uint32_t(merged.vertices.size());
      for (size_t v = 0; v < md.vertices.size(); ++v) {
        Vertex x = md.vertices[v];
        x.position = glm::vec3(o.transform * glm::vec4(x.position, 1.0f)) - center;
        x.normal = glm::normalize(nm * x.normal);
        x.tangent = glm::vec4(glm::normalize(m3 * glm::vec3(x.tangent)), x.tangent.w * (mirrored ? -1.0f : 1.0f));
        merged.vertices.push_back(x);
        if (colored) merged.colors.push_back(md.colors.empty() ? glm::vec4(1.0f) : md.colors[v]);
      }
      for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {  // espelhado: inverte a ordem (a frente continua a mesma)
        merged.indices.push_back(base + md.indices[t]);
        merged.indices.push_back(base + md.indices[t + (mirrored ? 2 : 1)]);
        merged.indices.push_back(base + md.indices[t + (mirrored ? 1 : 2)]);
      }
    }
    const size_t tris = merged.indices.size() / 3;
    trisIn += tris;
    // Fusões grandes: 1/4 dos triângulos (erro quádrico). Pequenas (caixas de prédios) ficam como estão: o ganho
    // delas é trocar dezenas de instâncias por uma.
    MeshData proxy = tris > 2000 ? simplifyMesh(merged, uint32_t(tris / 4)) : std::move(merged);
    trisOut += proxy.indices.size() / 3;
    const uint32_t mesh = addMesh(std::move(proxy));
    const uint32_t p = addObject(mesh, glm::translate(glm::mat4(1.0f), center), key.second);
    objects[p].hlodProxy = true;
    objects[p].hlodCell = cell;
    for (uint32_t i : list) objects[i].hlodCell = cell;
    ++proxies;
    members += list.size();
  }
  LOG_INFO("HLOD: %zu células, %zu proxies substituem %zu objetos ao longe (%zu → %zu triângulos)", hlodCells.size(),
           proxies, members, trisIn, trisOut);
}

// Leitor do disco em segundo plano: um arquivo com as malhas de todas as células (vértices, índices, cores e
// meshlets, uma atrás da outra) e uma thread que lê uma célula inteira por pedido.
struct StreamLoader {
  struct Record {
    uint64_t offset = 0;
    uint32_t vertices = 0, indices = 0, colors = 0, meshletWords = 0;
  };
  std::string path;
  std::ofstream out;
  std::vector<Record> records;               // por malha
  std::vector<std::vector<uint32_t>> cells;  // malhas de cada célula
  std::thread worker;
  std::mutex mu;
  std::condition_variable cv;
  std::deque<uint32_t> requests;
  std::vector<std::pair<uint32_t, std::vector<LoadedMesh>>> ready;
  bool quit = false;

  explicit StreamLoader(size_t meshCount) : records(meshCount) {
    static int serial = 0;
    path = (std::filesystem::temp_directory_path() /
            ("legacy_stream_" + std::to_string(uintptr_t(this)) + "_" + std::to_string(serial++) + ".pak")).string();
    out.open(path, std::ios::binary | std::ios::trunc);
    if (!out) FATAL("Streaming: não deu para criar %s", path.c_str());
  }
  ~StreamLoader() {
    {
      std::lock_guard<std::mutex> l(mu);
      quit = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
    std::remove(path.c_str());
  }
  template <class T> void put(const std::vector<T>& v) { out.write(reinterpret_cast<const char*>(v.data()), std::streamsize(v.size() * sizeof(T))); }
  void write(uint32_t mesh, const MeshData& d, const std::vector<uint32_t>& meshlets) {
    Record& r = records[mesh];
    r.offset = uint64_t(out.tellp());
    r.vertices = uint32_t(d.vertices.size());
    r.indices = uint32_t(d.indices.size());
    r.colors = uint32_t(d.colors.size());
    r.meshletWords = uint32_t(meshlets.size());
    put(d.vertices);
    put(d.indices);
    put(d.colors);
    put(meshlets);
  }
  void start(std::vector<std::vector<uint32_t>> cellMeshes) {
    out.close();
    cells = std::move(cellMeshes);
    worker = std::thread([this] { run(); });
  }
  template <class T> static void get(std::ifstream& in, std::vector<T>& v, uint32_t n) {
    v.resize(n);
    in.read(reinterpret_cast<char*>(v.data()), std::streamsize(size_t(n) * sizeof(T)));
  }
  void run() {
    std::ifstream in(path, std::ios::binary);
    for (;;) {
      uint32_t cell;
      {
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return quit || !requests.empty(); });
        if (quit) return;
        cell = requests.front();
        requests.pop_front();
      }
      std::vector<LoadedMesh> meshes;
      for (uint32_t m : cells[cell]) {
        const Record& r = records[m];
        LoadedMesh lm;
        lm.mesh = m;
        in.seekg(std::streamoff(r.offset));
        get(in, lm.data.vertices, r.vertices);
        get(in, lm.data.indices, r.indices);
        get(in, lm.data.colors, r.colors);
        get(in, lm.meshlets, r.meshletWords);
        if (!in) FATAL("Streaming: leitura falhou em %s", path.c_str());
        meshes.push_back(std::move(lm));
      }
      {
        std::lock_guard<std::mutex> l(mu);
        ready.emplace_back(cell, std::move(meshes));
      }
      cv.notify_all();
    }
  }
};

size_t Scene::textureBytes() const {
  size_t bytes = 0;
  for (size_t i = 0; i < textures.size(); ++i) {
    const TextureStream& ts = i < textureStreams.size() ? textureStreams[i] : TextureStream{};
    if (ts.streamed()) {
      for (size_t m = ts.base; m < ts.size.size(); ++m) bytes += ts.size[m];
    } else {
      // BC7/BC5: 1 byte por texel; RGBA8: 4; mais 1/3 da cadeia de mips.
      const VkFormat f = textures[i].format;
      const size_t perTexel = (f == VK_FORMAT_BC7_SRGB_BLOCK || f == VK_FORMAT_BC7_UNORM_BLOCK || f == VK_FORMAT_BC5_UNORM_BLOCK) ? 1 : 4;
      bytes += size_t(textures[i].extent.width) * textures[i].extent.height * perTexel * 4 / 3;
    }
  }
  return bytes;
}

rhi::Image Scene::uploadTextureFrom(rhi::Context& ctx, const TextureStream& t, uint32_t base) {
  std::ifstream in(texturePack_, std::ios::binary);
  std::vector<std::vector<uint8_t>> mips(t.size.size() - base);
  for (size_t m = base; m < t.size.size(); ++m) {
    mips[m - base].resize(t.size[m]);
    in.seekg(std::streamoff(t.offset[m]));
    in.read(reinterpret_cast<char*>(mips[m - base].data()), t.size[m]);
  }
  if (!in) FATAL("Streaming de texturas: leitura falhou em %s", texturePack_.c_str());
  return rhi::createCompressedTexture(ctx, t.format, std::max(1u, t.width >> base), std::max(1u, t.height >> base), mips);
}

void Scene::updateTextureStreaming(rhi::Context& ctx, const uint32_t* need, uint32_t count) {
  for (size_t k = 0; k < textureGraveyard_.size();) {
    if (--textureGraveyard_[k].framesLeft == 0) {
      rhi::destroyImage(ctx, textureGraveyard_[k].image);
      textureGraveyard_[k] = textureGraveyard_.back();
      textureGraveyard_.pop_back();
    } else {
      ++k;
    }
  }
  if (texturePack_.empty()) return;
  // Nível pedido: o mais grosso que ainda tem os texels que a tela pediu.
  struct Want {
    float priority;
    uint32_t texture, base;
  };
  std::vector<Want> up;
  for (uint32_t i = 0; i < count && i < textureStreams.size(); ++i) {
    TextureStream& ts = textureStreams[i];
    if (!ts.streamed()) continue;
    const uint32_t full = std::max(ts.width, ts.height);
    uint32_t want = ts.coarsest;
    if (need[i] > 0)
      while (want > 0 && (full >> want) < need[i]) --want;
    if (want < ts.base) {
      up.push_back({float(full >> want) / float(full >> ts.base), i, want});
      ts.idleFrames = 0;
    } else if (want > ts.base) {
      // Devolve os mips finos só depois de ~5 s sem uso (não fica trocando de nível a cada passo da câmera).
      if (++ts.idleFrames > 300) {
        textureGraveyard_.push_back({textures[i], rhi::kFramesInFlightMax + 1});
        textures[i] = uploadTextureFrom(ctx, ts, want);
        ts.base = want;
        ts.idleFrames = 0;
        ++texturesVersion;
      }
    } else {
      ts.idleFrames = 0;
    }
  }
  // Sobe primeiro as que mais ganham (maior salto de resolução), até o orçamento do frame.
  std::sort(up.begin(), up.end(), [](const Want& a, const Want& b) { return a.priority > b.priority; });
  double budget = double(textureStreamBudgetMB) * 1048576.0;
  for (const Want& w : up) {
    TextureStream& ts = textureStreams[w.texture];
    size_t bytes = 0;
    for (size_t m = w.base; m < ts.size.size(); ++m) bytes += ts.size[m];
    if (budget <= 0.0) break;
    textureGraveyard_.push_back({textures[w.texture], rhi::kFramesInFlightMax + 1});
    textures[w.texture] = uploadTextureFrom(ctx, ts, w.base);
    ts.base = w.base;
    budget -= double(bytes);
    ++texturesVersion;
  }
}

void Scene::makeResident(uint32_t mi, const MeshData& data, const std::vector<uint32_t>& meshlets) {
  rhi::Context& ctx = *ctx_;
  GpuMesh& gm = meshes[mi];
  VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (ctx.rayTracing) geometryUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  const VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex), ibytes = data.indices.size() * sizeof(uint32_t);
  gm.vertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage);
  gm.indices = rhi::createBufferWithData(ctx, data.indices.data(), ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | geometryUsage);
  if (ctx.rayTracing) {
    rhi::BlasInput in{gm.vertices.address, gm.vertexCount, sizeof(Vertex), gm.indices.address, gm.indexCount};
    gm.blas = rhi::buildBlas(ctx, in, false);
  }
  gm.resident = true;
  residentBytes_ += vbytes + ibytes + (ctx.rayTracing ? gm.blas.storage.size : 0) + uploadMeshlets(ctx, gm, meshlets) +
                    uploadColors(ctx, gm, data);
  for (uint32_t o : objectsOfMesh_[mi]) {
    instanceUpdates_.push_back(o);  // endereços novos da malha no buffer de instâncias
    // Reescreve o objeto na GPU neste frame (visível de novo). Direto em changed_: stream() roda depois de update(),
    // e um setTransform aqui seria tratado como "movido fora do update" e descartado no próximo.
    changed_.push_back(o);
  }
}

void Scene::evict(uint32_t mi) {
  GpuMesh& gm = meshes[mi];
  gm.resident = false;
  for (uint32_t o : objectsOfMesh_[mi]) {
    instanceUpdates_.push_back(o);  // volta a apontar para a malha vazia antes que esta seja destruída
    changed_.push_back(o);          // objeto oculto (o TLAS sai pelo novo layout)
  }
  pendingDestroy_.push_back({mi, rhi::kFramesInFlightMax + 1});  // a GPU pode ainda estar usando
}

bool Scene::stream(rhi::Context& ctx, glm::vec3 camera) {
  bool changed = false;
  // Destruições adiadas: só quando nenhum frame em voo usa mais a malha.
  for (size_t k = 0; k < pendingDestroy_.size();) {
    PendingDestroy& p = pendingDestroy_[k];
    if (--p.framesLeft > 0 || meshes[p.mesh].resident) {
      if (meshes[p.mesh].resident) {  // voltou a ser usada antes de destruir: nada a fazer
        pendingDestroy_[k] = pendingDestroy_.back();
        pendingDestroy_.pop_back();
        continue;
      }
      ++k;
      continue;
    }
    GpuMesh& gm = meshes[p.mesh];
    residentBytes_ -= gm.vertices.size + gm.indices.size + (ctx.rayTracing ? gm.blas.storage.size : 0) + gm.meshlets.size + gm.colors.size;
    rhi::destroyBuffer(ctx, gm.vertices);
    rhi::destroyBuffer(ctx, gm.meshlets);
    rhi::destroyBuffer(ctx, gm.colors);
    rhi::destroyBuffer(ctx, gm.indices);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, gm.blas);
    pendingDestroy_[k] = pendingDestroy_.back();
    pendingDestroy_.pop_back();
  }
  // Células a carregar (mais perto primeiro) e a descarregar.
  std::vector<std::pair<float, uint32_t>> toLoad;
  for (uint32_t c = 0; c < streamCells.size(); ++c) {
    StreamCell& cell = streamCells[c];
    const float d = std::max(glm::length(cell.center - camera) - cell.radius, 0.0f);
    if (!cell.resident && d < streamLoadDistance) toLoad.push_back({d, c});
    if (cell.resident && d > streamUnloadDistance) {
      for (uint32_t m : cell.meshes) evict(m);
      cell.resident = false;
      cellRequested_[c] = 0;
      changed = true;
    }
  }
  std::sort(toLoad.begin(), toLoad.end());
  if (!loader_) return changed;  // cena sem células de streaming
  // Leituras do disco: pede as células que faltam (a mais perto primeiro) e recolhe as que a thread já leu.
  StreamLoader& L = *loader_;
  auto collect = [&] {
    std::lock_guard<std::mutex> l(L.mu);
    for (auto& [c, data] : L.ready) loadedCells_[c] = std::move(data);
    L.ready.clear();
  };
  {
    std::lock_guard<std::mutex> l(L.mu);
    for (const auto& [d, c] : toLoad)
      if (!cellRequested_[c]) {
        cellRequested_[c] = 1;
        L.requests.push_back(c);
      }
  }
  L.cv.notify_all();
  collect();
  // Esperar no mesmo frame: a célula onde a câmera está (senão o chão some), tudo na primeira chamada (a "tela de
  // carregamento": sem ela o 1º frame sai sem os prédios de longe e a auto-exposição demora a voltar) ou tudo sempre,
  // no modo de referência.
  const bool waitAll = streamSync || !streamStarted_;
  streamStarted_ = true;
  for (const auto& [d, c] : toLoad) {
    if (!(waitAll || d <= 0.0f)) continue;
    std::unique_lock<std::mutex> l(L.mu);
    L.cv.wait(l, [&] {
      for (auto& [rc, data] : L.ready) loadedCells_[rc] = std::move(data);
      L.ready.clear();
      return loadedCells_.count(c) > 0;
    });
  }
  double budget = double(streamBudgetMB) * 1048576.0;
  for (const auto& [d, c] : toLoad) {
    auto it = loadedCells_.find(c);
    if (it == loadedCells_.end()) continue;  // ainda no disco
    StreamCell& cell = streamCells[c];
    size_t bytes = 0;
    for (const LoadedMesh& lm : it->second) bytes += lm.data.vertices.size() * sizeof(Vertex) + lm.data.indices.size() * 4;
    if (budget <= 0.0 && d > 0.0) break;  // orçamento do frame acabou (a célula onde a câmera está sempre carrega)
    for (const LoadedMesh& lm : it->second)
      if (!meshes[lm.mesh].resident) makeResident(lm.mesh, lm.data, lm.meshlets);
    cell.resident = true;
    loadedCells_.erase(it);  // a cópia na RAM sai assim que sobe
    budget -= double(bytes);
    changed = true;
  }
  // Lidas mas não mais desejadas (a câmera se afastou antes de subir): descarta, um novo pedido relê.
  for (auto it = loadedCells_.begin(); it != loadedCells_.end();) {
    const StreamCell& cell = streamCells[it->first];
    if (std::max(glm::length(cell.center - camera) - cell.radius, 0.0f) > streamUnloadDistance) {
      cellRequested_[it->first] = 0;
      it = loadedCells_.erase(it);
    } else {
      ++it;
    }
  }
  if (changed) {
    rebuildTlasLayout();
    size_t resident = 0;
    for (const StreamCell& cell : streamCells) resident += cell.resident ? 1 : 0;
    LOG_INFO("Streaming: %zu de %zu células na GPU, %.1f MB (malhas + BLAS)", resident, streamCells.size(),
             double(residentBytes_) / 1048576.0);
  }
  return changed;
}

uint32_t Scene::bakeCrowdClip(int modelIndex, uint32_t frames) {
  Model& m = models[size_t(modelIndex)];
  if (m.skinnedObjects.empty()) FATAL("bakeCrowdClip: o modelo não tem malha com esqueleto");
  const uint32_t object = m.skinnedObjects[0];
  const MeshData src = cpuMeshes_[objects[object].mesh];
  const Animation* a = (m.animation >= 0 && size_t(m.animation) < m.animations.size()) ? &m.animations[size_t(m.animation)] : nullptr;
  CrowdClip clip;
  clip.frames = frames;
  clip.duration = a && a->duration > 0.0f ? a->duration : 1.0f;
  clip.material = objects[object].material;
  clip.firstMesh = uint32_t(cpuMeshes_.size());  // as poses entram em seguida
  const glm::mat4 savedRoot = m.root;
  const float savedTime = m.time;
  m.root = glm::mat4(1.0f);
  for (uint32_t k = 0; k < frames; ++k) {
    m.time = clip.duration * float(k) / float(frames);
    animateModel(m, 0.0f);
    const auto& J = m.jointMatrices[size_t(src.skin)];
    MeshData pose;
    pose.indices = src.indices;
    pose.vertices = src.vertices;
    for (size_t v = 0; v < src.vertices.size(); ++v) {  // pele linear (a mesma conta de shaders/skinning.comp)
      glm::mat4 S(0.0f);
      for (int j = 0; j < 4; ++j) S += J[src.joints[v][j]] * src.weights[v][j];
      Vertex& out = pose.vertices[v];
      out.position = glm::vec3(S * glm::vec4(src.vertices[v].position, 1.0f));
      out.normal = glm::normalize(glm::mat3(S) * src.vertices[v].normal);
      out.tangent = glm::vec4(glm::normalize(glm::mat3(S) * glm::vec3(src.vertices[v].tangent)), src.vertices[v].tangent.w);
    }
    addMesh(std::move(pose));
  }
  // LOD das poses: um agrupamento de vértices calculado na pose 0 e aplicado a todas (mesma topologia em cada nível,
  // o vertex shader puxa o vértice pelo índice). Para a multidão de longe (milhares de pessoas).
  const uint32_t tris = uint32_t(src.indices.size() / 3);
  if (meshLods && tris >= 400) {
    for (int k = 0; k < 2; ++k) {
      std::vector<uint32_t> groups;
      const MeshData lod0 = simplifyMesh(cpuMeshes_[clip.firstMesh], tris / (k == 0 ? 4 : 8), &groups);
      clip.lodFirstMesh[k] = uint32_t(cpuMeshes_.size());
      for (uint32_t f = 0; f < frames; ++f) {
        MeshData lod = applyVertexGroups(cpuMeshes_[clip.firstMesh + f], groups, lod0);  // cópia antes do addMesh
        addMesh(std::move(lod));
      }
    }
  }
  m.root = savedRoot;
  m.time = savedTime;
  animateModel(m, 0.0f);
  crowdClips.push_back(clip);
  return uint32_t(crowdClips.size() - 1);
}

uint32_t Scene::addCrowdAgent(uint32_t clip, const glm::mat4& transform, float phase) {
  const CrowdClip& c = crowdClips[clip];
  const uint32_t o = addObject(c.firstMesh, transform, c.material);
  objects[o].crowdClip = clip;
  objects[o].animPhase = phase;
  const uint32_t frame = uint32_t(std::floor(std::fmod(phase, c.duration) / c.duration * float(c.frames))) % c.frames;
  objects[o].animFrame = objects[o].prevAnimFrame = frame;
  crowdObjects_.push_back(o);
  return o;
}

void Scene::addDecal(DecalType type, glm::vec3 center, glm::vec3 halfExtent, float yawRad, glm::vec4 color) {
  glm::mat4 m = glm::translate(glm::mat4(1.0f), center) * glm::rotate(glm::mat4(1.0f), yawRad, glm::vec3(0, 1, 0)) *
                glm::scale(glm::mat4(1.0f), halfExtent);
  decals.push_back({glm::inverse(m), color, uint32_t(type), float(decals.size()) * 0.618034f, -1, 0.6f});
}

void Scene::addWallDecal(int32_t texture, glm::vec3 center, glm::vec3 wallNormal, float width, float height, float roughness) {
  const glm::vec3 z = glm::normalize(wallNormal);
  const glm::vec3 x = glm::normalize(glm::cross(std::abs(z.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0), z));
  const glm::vec3 y = glm::cross(z, x);
  glm::mat4 m(1.0f);
  m[0] = glm::vec4(-x * 0.5f * width, 0.0f);  // u cresce para a direita de quem olha a parede
  m[1] = glm::vec4(y * 0.5f * height, 0.0f);
  m[2] = glm::vec4(z * 0.15f, 0.0f);         // projeta 15 cm para dentro e para fora da parede
  m[3] = glm::vec4(center, 1.0f);
  Decal d{glm::inverse(m), glm::vec4(1.0f), uint32_t(kDecalTexture), 0.0f, texture, roughness};
  decals.push_back(d);
}

void Scene::addEmitter(ParticleType type, glm::vec3 position, float radius, float scale, uint32_t count) {
  emitters.push_back({position, uint32_t(type), radius, scale, particleCount, count});
  particleCount += count;
  if (type == kParticleExplosion) {  // clarão: luz forte que decai em ~0,3 s, no mesmo ciclo das partículas
    const uint32_t light = addLight({position, 1.0f * scale, glm::vec3(0.0f), 80.0f * scale});
    addAnimator([light, scale](Scene& s, float t, float) {
      const float k = std::fmod(t, 5.0f);  // == EXPLOSION_PERIOD em shaders/particles.glsl
      s.lights[light].intensity = glm::vec3(1.0f, 0.6f, 0.25f) * (400000.0f * scale * std::exp(-k * 7.0f));
    });
    return;
  }
  if (type != kParticleFire) return;
  // Chama ~1800 K: ~1000 cd por metro de altura, centro um pouco acima da base, tremulando.
  const uint32_t light = addLight({position + glm::vec3(0.0f, 0.5f * scale, 0.0f), 0.3f * scale,
                                   glm::vec3(1.0f, 0.55f, 0.2f) * 1000.0f * scale, 30.0f * scale});
  const glm::vec3 base = lights[light].intensity;
  addAnimator([light, base](Scene& s, float t, float) {
    const float flicker = 0.85f + 0.1f * std::sin(t * 13.0f) + 0.05f * std::sin(t * 31.7f + 1.3f);
    s.lights[light].intensity = base * flicker;
  });
}

glm::vec3 lightCenter(const PointLight& l) {
  return l.type == kLightTriangle ? l.position + (l.direction + l.edge2) / 3.0f : l.position;
}

void lightBounds(const PointLight& l, glm::vec3& lo, glm::vec3& hi) {
  if (l.type == kLightTriangle) {
    const glm::vec3 a = l.position, b = l.position + l.direction, c = l.position + l.edge2;
    lo = glm::min(a, glm::min(b, c));
    hi = glm::max(a, glm::max(b, c));
  } else {
    lo = l.position - glm::vec3(l.radius);
    hi = l.position + glm::vec3(l.radius);
  }
}

float lightPower(const PointLight& l) {
  const float lum = 0.2126f * l.intensity.r + 0.7152f * l.intensity.g + 0.0722f * l.intensity.b;
  return std::max(lum, 0.0f) * (l.type == kLightTriangle ? l.area : 1.0f);
}

namespace {
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

// Versão pequena (até 128x128, cor linear) de uma textura emissiva, para estimar a potência de cada triângulo.
struct SmallTexture {
  uint32_t w = 1, h = 1;
  std::vector<glm::vec3> px{glm::vec3(1.0f)};
  glm::vec3 at(glm::vec2 uv) const {
    const float fx = uv.x - std::floor(uv.x), fy = uv.y - std::floor(uv.y);  // repetição
    return px[size_t(std::min(uint32_t(fy * float(h)), h - 1)) * w + std::min(uint32_t(fx * float(w)), w - 1)];
  }
};
SmallTexture smallTexture(const TextureData& t) {
  SmallTexture s;
  std::vector<uint8_t> decoded;
  const uint8_t* p = nullptr;
  uint32_t w = t.width, h = t.height;
  if (t.precompressed.format != VK_FORMAT_UNDEFINED) {
    uint32_t level = 0;
    while (level + 1 < t.precompressed.mips.size() && std::max(w, h) > 128) {
      ++level;
      w = std::max(1u, w / 2);
      h = std::max(1u, h / 2);
    }
    decoded = decodeBc7(t.precompressed, level);
    p = decoded.data();
  } else {
    p = t.rgba.data();
  }
  if (!p || w == 0 || h == 0) return s;
  const uint32_t step = std::max(1u, std::max(w, h) / 128);  // RGBA grande: amostra de 1 em `step`
  s.w = std::max(1u, w / step);
  s.h = std::max(1u, h / step);
  s.px.resize(size_t(s.w) * s.h);
  for (uint32_t y = 0; y < s.h; ++y)
    for (uint32_t x = 0; x < s.w; ++x) {
      const uint8_t* q = p + (size_t(std::min(y * step, h - 1)) * w + std::min(x * step, w - 1)) * 4;
      for (int c = 0; c < 3; ++c) s.px[size_t(y) * s.w + x][c] = t.srgb ? srgbToLinear(q[c] / 255.0f) : q[c] / 255.0f;
    }
  return s;
}
}  // namespace

void Scene::writeMeshLights(const MeshLightSource& src) {
  const glm::mat4& m = objects[src.object].transform;
  const bool mirrored = glm::determinant(glm::mat3(m)) < 0.0f;  // espelhamento inverte a frente dos triângulos
  for (size_t t = 0; t < src.primitives.size(); ++t) {
    PointLight& l = lights[src.firstLight + t];
    const glm::vec3 a = glm::vec3(m * glm::vec4(src.local[3 * t], 1.0f));
    const glm::vec3 b = glm::vec3(m * glm::vec4(src.local[3 * t + 1], 1.0f));
    const glm::vec3 c = glm::vec3(m * glm::vec4(src.local[3 * t + 2], 1.0f));
    l.position = a;
    l.direction = mirrored ? c - a : b - a;
    l.edge2 = mirrored ? b - a : c - a;
    l.area = 0.5f * glm::length(glm::cross(b - a, c - a));
  }
}

void Scene::buildMeshLights() {
  std::vector<SmallTexture> small(cpuTextures_.size());
  std::vector<uint8_t> smallReady(cpuTextures_.size(), 0);
  meshLightSourceOf_.assign(objects.size(), -1);
  size_t triangles = 0;
  for (uint32_t i = 0; i < objects.size(); ++i) {
    Material& mat = materials[objects[i].material];
    if (mat.flags & kMaterialLightGeometry) continue;  // já é a forma visível de uma luz analítica
    if (objects[i].mesh < meshCellPending_.size() && meshCellPending_[objects[i].mesh] >= 0) continue;  // streaming
    const MeshData& md = cpuMeshes_[objects[i].mesh];
    if (md.skinModel >= 0 || md.indices.empty()) continue;  // malha deformável: ainda não (raro em emissivos)
    const auto lumOf = [](glm::vec3 c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; };
    if (!(lumOf(mat.emissive) > 0.0f)) continue;
    const SmallTexture* tex = nullptr;
    if (mat.emissiveTex >= 0 && size_t(mat.emissiveTex) < cpuTextures_.size()) {
      const size_t ti = size_t(mat.emissiveTex);
      if (!smallReady[ti]) {
        small[ti] = smallTexture(cpuTextures_[ti]);
        smallReady[ti] = 1;
      }
      tex = &small[ti];
    }
    MeshLightSource src{i, uint32_t(lights.size()), {}, {}};
    std::vector<glm::vec3> estimate;  // radiância média estimada de cada triângulo (vértices + centro na textura)
    glm::vec3 sum(0.0f);
    for (uint32_t t = 0; t + 2 < md.indices.size(); t += 3) {
      const Vertex &va = md.vertices[md.indices[t]], &vb = md.vertices[md.indices[t + 1]], &vc = md.vertices[md.indices[t + 2]];
      if (glm::length(glm::cross(vb.position - va.position, vc.position - va.position)) <= 0.0f) continue;  // degenerado
      glm::vec3 Le = mat.emissive;
      if (tex)
        Le *= 0.25f * (tex->at(va.uv) + tex->at(vb.uv) + tex->at(vc.uv) + tex->at((va.uv + vb.uv + vc.uv) / 3.0f));
      src.local.insert(src.local.end(), {va.position, vb.position, vc.position});
      src.primitives.push_back(t / 3);
      estimate.push_back(Le);
      sum += Le;
    }
    if (src.primitives.empty() || !(lumOf(sum) > 0.0f)) continue;
    mat.flags |= kMaterialEmissionSampled;
    // Piso de 1% da média: um triângulo que a estimativa vê escuro ainda pode ter detalhe emissivo por dentro, e
    // potência zero o tornaria impossível de amostrar (viés). O valor exato vem da textura no shader.
    const glm::vec3 floorLe = 0.01f * sum / float(estimate.size());
    for (size_t t = 0; t < src.primitives.size(); ++t) {
      PointLight l;
      l.type = kLightTriangle;
      l.object = i;
      l.primitive = src.primitives[t];
      l.intensity = glm::max(estimate[t], floorLe);
      l.radius = 0.0f;
      lights.push_back(l);
    }
    writeMeshLights(src);
    // Sem alcance: a janela de alcance das luzes pontuais (corte suave artístico) já tira luz bem antes do corte, e
    // numa luz de malha isso seria viés — medido: −16% nas paredes da cena mesh-lights. A BVH de luzes já faz as
    // luzes distantes e fracas quase nunca serem escolhidas.
    for (size_t t = 0; t < src.primitives.size(); ++t) lights[src.firstLight + t].range = 1e30f;
    triangles += src.primitives.size();
    meshLightSourceOf_[i] = int32_t(meshLightSources_.size());
    meshLightSources_.push_back(std::move(src));
  }
  if (triangles)
    LOG_INFO("Luzes de malha: %zu triângulos emissivos em %zu objetos", triangles, meshLightSources_.size());
}

void Scene::bounds(glm::vec3& lo, glm::vec3& hi) const {
  lo = glm::vec3(1e30f);
  hi = glm::vec3(-1e30f);
  for (const auto& o : objects) {
    if (o.mesh >= cpuMeshes_.size()) continue;
    for (const auto& v : cpuMeshes_[o.mesh].vertices) {
      glm::vec3 p = glm::vec3(o.transform * glm::vec4(v.position, 1.0f));
      lo = glm::min(lo, p);
      hi = glm::max(hi, p);
    }
  }
}

void Scene::modelBounds(int model, glm::vec3& lo, glm::vec3& hi) const {
  lo = glm::vec3(1e30f);
  hi = glm::vec3(-1e30f);
  const Model& m = models[size_t(model)];
  auto add = [&](glm::vec3 p) { lo = glm::min(lo, p); hi = glm::max(hi, p); };
  for (auto [node, object] : m.rigidObjects)
    for (const auto& v : cpuMeshes_[objects[object].mesh].vertices) add(glm::vec3(objects[object].transform * glm::vec4(v.position, 1)));
  for (uint32_t object : m.skinnedObjects) {
    const MeshData& d = cpuMeshes_[objects[object].mesh];
    const auto& joints = m.jointMatrices[size_t(d.skin)];
    for (size_t i = 0; i < d.vertices.size(); ++i) {
      glm::mat4 skin(0.0f);
      for (int k = 0; k < 4; ++k) skin += joints[d.joints[i][k]] * d.weights[i][k];
      add(glm::vec3(m.root * skin * glm::vec4(d.vertices[i].position, 1)));
    }
  }
}

void Scene::upload(rhi::Context& ctx) {
  for (auto& m : models) animateModel(m, 0.0f);  // pose inicial
  for (auto& o : objects) o.prevTransform = o.transform;
  if (meshLights) buildMeshLights();
  buildHlod();  // depois das luzes de malha: o proxy não é luz (os membros continuam no ray tracing)
  // LODs: malhas estáticas com mais de 3 mil triângulos ganham versões com 1/4 e 1/16 dos triângulos.
  std::vector<std::array<int, 2>> lodOf(cpuMeshes_.size(), {-1, -1});
  std::vector<float> meshRadius(cpuMeshes_.size(), 0.0f);
  if (meshLods) {
    const size_t original = cpuMeshes_.size();
    std::vector<uint8_t> excluded(original, 0);
    for (const Object& o : objects)
      if (o.crowdClip != ~0u || (materials[o.material].flags & (kMaterialWater | kMaterialHair))) excluded[o.mesh] = 1;
    for (const CrowdClip& c : crowdClips) {  // poses assadas: o LOD delas é o da multidão (bakeCrowdClip)
      for (uint32_t f = 0; f < c.frames; ++f) excluded[c.firstMesh + f] = 1;
      for (uint32_t l : c.lodFirstMesh)
        if (l != ~0u)
          for (uint32_t f = 0; f < c.frames; ++f) excluded[l + f] = 1;
    }
    size_t made = 0;
    for (size_t i = 0; i < original; ++i) {
      const MeshData& md = cpuMeshes_[i];
      const uint32_t tris = uint32_t(md.indices.size() / 3);
      const bool streamed = i < meshCellPending_.size() && meshCellPending_[i] >= 0;
      if (excluded[i] || streamed || tris < 3000 || md.skinModel >= 0 || md.clothWidth > 0) continue;
      glm::vec3 lo(1e30f), hi(-1e30f);
      for (const Vertex& v : md.vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
      }
      meshRadius[i] = 0.5f * glm::length(hi - lo);
      MeshData l1 = simplifyMesh(md, tris / 4);
      MeshData l2 = simplifyMesh(md, tris / 16);  // antes de addMesh: ele faz cpuMeshes_ crescer e invalida `md`
      lodOf[i][0] = int(addMesh(std::move(l1)));
      lodOf[i][1] = int(addMesh(std::move(l2)));
      ++made;
    }
    if (made) LOG_INFO("LODs: %zu malhas com 2 níveis simplificados", made);
    // Multidões: o nível de cada clipe vem das poses simplificadas (bakeCrowdClip); a troca é pela pose 0.
    for (const CrowdClip& c : crowdClips) {
      if (c.lodFirstMesh[0] == ~0u) continue;
      glm::vec3 lo(1e30f), hi(-1e30f);
      for (const Vertex& v : cpuMeshes_[c.firstMesh].vertices) {
        lo = glm::min(lo, v.position);
        hi = glm::max(hi, v.position);
      }
      // O dobro da distância de troca das malhas comuns: o agrupamento junta braço e tronco, e a 25 raios (~22 m)
      // a silhueta mais cheia ainda aparecia (medido na cena crowd).
      meshRadius[c.firstMesh] = glm::length(hi - lo);
      lodOf[c.firstMesh] = {int(c.lodFirstMesh[0]), int(c.lodFirstMesh[1])};
    }
  }

  VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (ctx.rayTracing) geometryUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  const VkBufferUsageFlags skinnedUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

  size_t triangles = 0, skinnedCount = 0;
  VkDeviceSize scratchTotal = 0;
  ctx_ = &ctx;
  meshCellPending_.resize(cpuMeshes_.size(), -1);
  for (int c : meshCellPending_)
    if (c >= 0 && !loader_) loader_ = std::make_shared<StreamLoader>(cpuMeshes_.size());
  for (auto& data : cpuMeshes_) {
    GpuMesh gm;
    const size_t meshIndex = meshes.size();
    if (meshCellPending_[meshIndex] >= 0) {
      // Malha de célula de streaming: os dados ficam na RAM; sobe para a GPU quando a câmera chega perto.
      gm.cell = meshCellPending_[meshIndex];
      gm.resident = false;
      gm.vertexCount = uint32_t(data.vertices.size());
      gm.indexCount = uint32_t(data.indices.size());
      gm.boundsMin = glm::vec3(1e30f);
      gm.boundsMax = glm::vec3(-1e30f);
      for (const auto& v : data.vertices) {
        gm.boundsMin = glm::min(gm.boundsMin, v.position);
        gm.boundsMax = glm::max(gm.boundsMax, v.position);
      }
      triangles += data.indices.size() / 3;
      loader_->write(uint32_t(meshIndex), data, meshletBlob(ctx, gm, data));
      data = MeshData{};  // fica só no disco
      meshes.push_back(gm);
      continue;
    }
    bool skinned = !data.joints.empty() && data.skinModel >= 0 && data.skin >= 0;
    const bool cloth = data.clothWidth > 0, hair = data.hairStrands > 0;
    VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex);
    VkBufferUsageFlags vusage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage | ((skinned || cloth || hair) ? skinnedUsage : 0);
    gm.vertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, vusage);
    gm.indices = rhi::createBufferWithData(ctx, data.indices.data(), data.indices.size() * sizeof(uint32_t),
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | geometryUsage);
    gm.vertexCount = uint32_t(data.vertices.size());
    gm.indexCount = uint32_t(data.indices.size());
    if (skinned) {
      gm.skinned = true;
      gm.skinModel = data.skinModel;
      gm.skin = data.skin;
      gm.restVertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes,
                                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
      std::vector<SkinVertex> sv(data.vertices.size());
      for (size_t i = 0; i < sv.size(); ++i) sv[i] = {data.joints[i], data.weights[i]};
      gm.skinData = rhi::createBufferWithData(ctx, sv.data(), sv.size() * sizeof(SkinVertex),
                                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT);
      gm.prevVertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, vusage);
      gm.jointOffset = totalJoints;
      gm.jointCount = uint32_t(models[size_t(data.skinModel)].skins[size_t(data.skin)].joints.size());
      totalJoints += gm.jointCount;
      ++skinnedCount;
    }
    if (cloth) {
      // cloth.comp simula um pano num grupo de trabalho: até 1.024 pontos em memória compartilhada.
      if (data.vertices.size() > 1024) FATAL("Tecido com %zu pontos (máximo 1.024 por pano)", data.vertices.size());
      gm.cloth = true;
      gm.clothWidth = data.clothWidth;
      gm.clothHeight = data.clothHeight;
      gm.clothSpacing = data.clothSpacing;
      gm.clothPinned = data.clothPinned;
      for (int k = 0; k < 4; ++k) gm.clothColliders[k] = data.clothColliders[k];
      std::vector<glm::vec4> p(data.vertices.size());
      for (size_t i = 0; i < p.size(); ++i) p[i] = glm::vec4(data.vertices[i].position, 0.0f);
      const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      gm.clothPos = rhi::createBufferWithData(ctx, p.data(), p.size() * sizeof(glm::vec4), su);
      gm.clothPrev = rhi::createBufferWithData(ctx, p.data(), p.size() * sizeof(glm::vec4), su);
      gm.prevVertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, vusage);
    }
    if (hair) {
      gm.hair = true;
      gm.hairStrands = data.hairStrands;
      gm.hairPoints = data.hairPoints;
      std::vector<glm::vec4> rest(size_t(data.hairStrands) * data.hairPoints);
      for (size_t i = 0; i < rest.size(); ++i)  // centro da fita (média dos dois lados), meia largura
        rest[i] = glm::vec4(0.5f * (data.vertices[2 * i].position + data.vertices[2 * i + 1].position), data.vertices[2 * i].tangent.w);
      const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC: Renderer::dumpHair
      gm.hairRest = rhi::createBufferWithData(ctx, rest.data(), rest.size() * sizeof(glm::vec4), su);
      gm.hairPos = rhi::createBuffer(ctx, rest.size() * sizeof(glm::vec4), su, rhi::MemoryUsage::GpuOnly);
      gm.hairPrev = rhi::createBuffer(ctx, rest.size() * sizeof(glm::vec4), su, rhi::MemoryUsage::GpuOnly);
      gm.prevVertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, vusage);
    }
    if (ctx.rayTracing) {
      rhi::BlasInput in{gm.vertices.address, gm.vertexCount, sizeof(Vertex), gm.indices.address, gm.indexCount};
      gm.blas = rhi::buildBlas(ctx, in, skinned || cloth || hair);
      if (skinned || cloth || hair) {
        gm.updateScratchOffset = scratchTotal;
        scratchTotal += rhi::blasUpdateScratchSize(ctx, in);
      }
    }
    triangles += data.indices.size() / 3;
    uploadMeshlets(ctx, gm, meshletBlob(ctx, gm, data));
    uploadColors(ctx, gm, data);
    gm.boundsMin = glm::vec3(1e30f);
    gm.boundsMax = glm::vec3(-1e30f);
    for (const auto& v : data.vertices) {
      gm.boundsMin = glm::min(gm.boundsMin, v.position);
      gm.boundsMax = glm::max(gm.boundsMax, v.position);
    }
    const size_t mi = meshes.size();
    if (mi < lodOf.size() && lodOf[mi][0] >= 0) {
      // Troca quando a malha ocupa pouco da tela: o nível 1 a 25 raios de distância, o 2 a 70 (com ~60° de campo de
      // visão, o objeto cobre ~4% e ~1,5% da altura da imagem).
      for (int k = 0; k < 2; ++k) {
        gm.lod[k] = lodOf[mi][k];
        gm.lodDistance[k] = meshRadius[mi] * (k == 0 ? 25.0f : 70.0f);
      }
    }
    meshes.push_back(gm);
  }
  cpuMeshes_.clear();
  if (loader_) {  // as células de streaming agora só existem no disco: a thread de leitura começa
    std::vector<std::vector<uint32_t>> cells;
    for (const StreamCell& c : streamCells) cells.push_back(c.meshes);
    loader_->start(std::move(cells));
    cellRequested_.assign(streamCells.size(), 0);
  }
  if (scratchTotal > 0)
    skinScratch = rhi::createBuffer(ctx, scratchTotal, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    rhi::MemoryUsage::GpuOnly);

  size_t texels = 0;
  VkFormatProperties bc7{};
  vkGetPhysicalDeviceFormatProperties(ctx.gpu, VK_FORMAT_BC7_SRGB_BLOCK, &bc7);
  const bool compress = compressTextures && !cpuTextures_.empty() &&
                        (bc7.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  if (compressTextures && !compress && !cpuTextures_.empty()) LOG_INFO("Texturas: GPU sem BC7, usando RGBA8");
  double bytesRaw = 0.0, bytesGpu = 0.0, psnrSum = 0.0, psnrMin = 99.0;
  uint32_t encoded = 0, precompressed = 0;
  auto t0 = std::chrono::steady_clock::now();
  // Streaming de texturas: todos os mips vão para um arquivo; a GPU começa só com os níveis de até 128 texels e
  // recebe os finos quando a tela pede (updateTextureStreaming).
  std::ofstream pack;
  if (textureStreaming && compress) {
    texturePack_ = (std::filesystem::temp_directory_path() /
                    ("legacy_textures_" + std::to_string(uintptr_t(this)) + ".pak")).string();
    pack.open(texturePack_, std::ios::binary | std::ios::trunc);
  }
  textureStreams.assign(cpuTextures_.size(), {});
  auto addCompressed = [&](VkFormat format, const CompressedTexture& ct) {
    const size_t i = textures.size();
    TextureStream& ts = textureStreams[i];
    if (pack.is_open() && ct.mips.size() > 1 && std::max(ct.width, ct.height) > 128) {
      ts.format = format;
      ts.width = ct.width;
      ts.height = ct.height;
      for (const auto& m : ct.mips) {
        ts.offset.push_back(uint64_t(pack.tellp()));
        ts.size.push_back(uint32_t(m.size()));
        pack.write(reinterpret_cast<const char*>(m.data()), std::streamsize(m.size()));
      }
      while (ts.coarsest + 1 < ct.mips.size() && (std::max(ct.width, ct.height) >> ts.coarsest) > 128) ++ts.coarsest;
      ts.base = ts.coarsest;
      const std::vector<std::vector<uint8_t>> tail(ct.mips.begin() + ts.base, ct.mips.end());
      textures.push_back(rhi::createCompressedTexture(ctx, format, std::max(1u, ct.width >> ts.base),
                                                      std::max(1u, ct.height >> ts.base), tail));
    } else {
      textures.push_back(rhi::createCompressedTexture(ctx, format, ct.width, ct.height, ct.mips));
    }
  };
  for (auto& t : cpuTextures_) {
    textureWrap.push_back({t.wrapU, t.wrapV});
    const double raw = double(t.width) * t.height * 4.0 * 4.0 / 3.0;  // RGBA8 com a cadeia de mips
    bytesRaw += raw;
    if (t.precompressed.format != VK_FORMAT_UNDEFINED && t.alphaCutoff >= 0.0f) {
      // DDS de recorte por alfa: os mips prontos perdem cobertura (folhagem some). Decodifica o nível 0 e refaz a
      // cadeia preservando a cobertura (comprimida uma vez, depois vem do cache).
      t.rgba = decodeBc7(t.precompressed, 0);
      t.precompressed = {};
    }
    if (t.precompressed.format != VK_FORMAT_UNDEFINED) {
      if (compress) {
        // Já em BC7 no disco: enviado sem recompressão (o formato segue o uso — cor sRGB ou dado linear).
        CompressedTexture& ct = t.precompressed;
        for (const auto& m : ct.mips) bytesGpu += double(m.size());
        ++precompressed;
        addCompressed(t.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, ct);
      } else {
        bytesGpu += raw;
        textures.push_back(rhi::createTexture(ctx, decodeBc7(t.precompressed, 0).data(), t.width, t.height, t.srgb));
      }
    } else if (compress) {
      ++encoded;
      CompressedTexture ct =
          compressTexture(t.rgba.data(), t.width, t.height, t.srgb, t.normalMap, textureCacheDir, t.alphaCutoff);
      for (const auto& m : ct.mips) bytesGpu += double(m.size());
      const double p = compressionPsnr(ct, t.rgba.data());
      psnrSum += p;
      psnrMin = std::min(psnrMin, p);
      if (p < 35.0)
        LOG_INFO("Textura BC7 com PSNR baixo: %s (%ux%u %s), %.1f dB", t.name.c_str(), t.width, t.height, t.srgb ? "sRGB" : "linear", p);
      addCompressed(ct.format, ct);
    } else {
      bytesGpu += raw;
      textures.push_back(rhi::createTexture(ctx, t.rgba.data(), t.width, t.height, t.srgb));
    }
    texels += size_t(t.width) * t.height;
  }
  if (compress)
    LOG_INFO("Texturas BC7/BC5: %.0f MB -> %.0f MB de memória de vídeo (%.1fx menos), %u comprimidas aqui (PSNR médio "
             "%.1f dB, pior %.1f dB), %u já em BC7 no disco, %.1f s",
             bytesRaw / 1048576.0, bytesGpu / 1048576.0, bytesRaw / std::max(bytesGpu, 1.0), encoded,
             encoded ? psnrSum / double(encoded) : 0.0, encoded ? psnrMin : 0.0, precompressed,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  cpuTextures_.clear();
  if (pack.is_open()) {
    pack.close();
    size_t streamed = 0;
    for (const TextureStream& ts : textureStreams) streamed += ts.streamed() ? 1 : 0;
    LOG_INFO("Streaming de texturas: %zu texturas começam nos mips de até 128 texels (%.0f MB na GPU)", streamed,
             double(textureBytes()) / 1048576.0);
  }

  if (materials.empty()) addMaterial({});
  materialBuffer = rhi::createBufferWithData(ctx, materials.data(), materials.size() * sizeof(Material),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  {
    std::vector<Decal> gpuDecals = decals;
    if (gpuDecals.empty()) gpuDecals.push_back({glm::mat4(1.0f), glm::vec4(0.0f), 0xFFFFFFFFu, 0.0f, -1, 0.0f});  // marcador
    decalBuffer = rhi::createBufferWithData(ctx, gpuDecals.data(), gpuDecals.size() * sizeof(Decal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
  // Malha vazia (endereços válidos para objetos fora da memória e BLAS das instâncias inativas).
  {
    const Vertex tri[3] = {{{0, 0, 0}, {0, 1, 0}, {0, 0}, {1, 0, 0, 1}}, {{1e-3f, 0, 0}, {0, 1, 0}, {0, 0}, {1, 0, 0, 1}},
                           {{0, 0, 1e-3f}, {0, 1, 0}, {0, 0}, {1, 0, 0, 1}}};
    const uint32_t idx[3] = {0, 1, 2};
    emptyVertices_ = rhi::createBufferWithData(ctx, tri, sizeof(tri), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage);
    emptyIndices_ = rhi::createBufferWithData(ctx, idx, sizeof(idx), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | geometryUsage);
  }
  std::vector<InstanceGpu> inst;
  for (auto& o : objects) {
    InstanceGpu g{0, 0, o.material, 0, 0};
    instanceAddresses(o.mesh, g.vertices, g.indices, g.colors);
    inst.push_back(g);
  }
  // TRANSFER_DST: o streaming reescreve os endereços das malhas que entram (Renderer::updateDynamicScene).
  instanceBuffer = rhi::createBufferWithData(ctx, inst.data(), inst.size() * sizeof(InstanceGpu),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  objectsOfMesh_.assign(meshes.size(), {});
  for (uint32_t i = 0; i < objects.size(); ++i) objectsOfMesh_[objects[i].mesh].push_back(i);

  if (ctx.rayTracing) {
    tlas.init(ctx, uint32_t(objects.size()));
    rebuildTlasLayout();
    for (uint32_t slot = 0; slot < rhi::DynamicTlas::kSlots; ++slot)
      for (uint32_t i = 0; i < objects.size(); ++i)
        if (tlasSlot_[i] >= 0) tlas.writeInstance(slot, uint32_t(tlasSlot_[i]), tlasInstance(i));
    ctx.immediateSubmit([&](VkCommandBuffer cmd) { tlas.build(cmd, 0, tlasCount_); });
  }
  size_t drawn = 0;
  for (auto& o : objects) drawn += meshes[o.mesh].indexCount / 3;
  LOG_INFO("Cena: %zu malhas (%zu tri, %zu com esqueleto), %zu objetos (%zu tri desenhados), %zu materiais, "
           "%zu texturas (%.1f Mtexels), %zu luzes, %zu modelos animados",
           meshes.size(), triangles, skinnedCount, objects.size(), drawn, materials.size(), textures.size(), texels / 1e6,
           lights.size(), models.size());
}

void Scene::destroy(rhi::Context& ctx) {
  loader_.reset();  // para a thread de leitura e apaga o arquivo
  for (DeadImage& d : textureGraveyard_) rhi::destroyImage(ctx, d.image);
  textureGraveyard_.clear();
  if (!texturePack_.empty()) std::remove(texturePack_.c_str());
  if (ctx.rayTracing) tlas.destroy();
  for (auto& m : meshes) {
    for (rhi::Buffer* b : {&m.vertices, &m.indices, &m.restVertices, &m.skinData, &m.prevVertices, &m.clothPos, &m.clothPrev,
                           &m.meshlets, &m.colors, &m.hairRest, &m.hairPos, &m.hairPrev})
      rhi::destroyBuffer(ctx, *b);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  for (auto& t : textures) rhi::destroyImage(ctx, t);
  for (rhi::Buffer* b : {&materialBuffer, &instanceBuffer, &skinScratch, &decalBuffer, &emptyVertices_, &emptyIndices_})
    rhi::destroyBuffer(ctx, *b);
  pendingDestroy_.clear();
  meshes.clear();
  textures.clear();
}

}  // namespace scene
