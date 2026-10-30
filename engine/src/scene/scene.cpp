#include "scene/scene.h"

#include <chrono>

#include "scene/texture_compress.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace scene {

namespace {
// Espelhado em shaders/scene.glsl (InstanceData).
struct InstanceGpu {
  uint64_t vertices, indices;
  uint32_t material, pad0;
  uint64_t pad1;
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
  return {&meshes[mesh].blas, objects[i].transform, i, opaque, mask};
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
  decals.push_back({glm::inverse(m), color, uint32_t(type), float(decals.size()) * 0.618034f});
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
  // LODs: malhas estáticas com mais de 3 mil triângulos ganham versões com 1/4 e 1/16 dos triângulos.
  std::vector<std::array<int, 2>> lodOf(cpuMeshes_.size(), {-1, -1});
  std::vector<float> meshRadius(cpuMeshes_.size(), 0.0f);
  if (meshLods) {
    const size_t original = cpuMeshes_.size();
    std::vector<uint8_t> excluded(original, 0);
    for (const Object& o : objects)
      if (o.crowdClip != ~0u || (materials[o.material].flags & kMaterialWater)) excluded[o.mesh] = 1;
    size_t made = 0;
    for (size_t i = 0; i < original; ++i) {
      const MeshData& md = cpuMeshes_[i];
      const uint32_t tris = uint32_t(md.indices.size() / 3);
      if (excluded[i] || tris < 3000 || md.skinModel >= 0 || md.clothWidth > 0) continue;
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
  }

  VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (ctx.rayTracing) geometryUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  const VkBufferUsageFlags skinnedUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

  size_t triangles = 0, skinnedCount = 0;
  VkDeviceSize scratchTotal = 0;
  for (auto& data : cpuMeshes_) {
    GpuMesh gm;
    bool skinned = !data.joints.empty() && data.skinModel >= 0 && data.skin >= 0;
    const bool cloth = data.clothWidth > 0;
    VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex);
    VkBufferUsageFlags vusage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage | ((skinned || cloth) ? skinnedUsage : 0);
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
      gm.cloth = true;
      gm.clothWidth = data.clothWidth;
      gm.clothHeight = data.clothHeight;
      gm.clothSpacing = data.clothSpacing;
      std::vector<glm::vec4> p(data.vertices.size());
      for (size_t i = 0; i < p.size(); ++i) p[i] = glm::vec4(data.vertices[i].position, 0.0f);
      const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
      gm.clothPos = rhi::createBufferWithData(ctx, p.data(), p.size() * sizeof(glm::vec4), su);
      gm.clothPrev = rhi::createBufferWithData(ctx, p.data(), p.size() * sizeof(glm::vec4), su);
      gm.prevVertices = rhi::createBufferWithData(ctx, data.vertices.data(), vbytes, vusage);
    }
    if (ctx.rayTracing) {
      rhi::BlasInput in{gm.vertices.address, gm.vertexCount, sizeof(Vertex), gm.indices.address, gm.indexCount};
      gm.blas = rhi::buildBlas(ctx, in, skinned || cloth);
      if (skinned || cloth) {
        gm.updateScratchOffset = scratchTotal;
        scratchTotal += rhi::blasUpdateScratchSize(ctx, in);
      }
    }
    triangles += data.indices.size() / 3;
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
        textures.push_back(rhi::createCompressedTexture(
            ctx, t.srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK, ct.width, ct.height, ct.mips));
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
      textures.push_back(rhi::createCompressedTexture(ctx, ct.format, ct.width, ct.height, ct.mips));
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

  if (materials.empty()) addMaterial({});
  materialBuffer = rhi::createBufferWithData(ctx, materials.data(), materials.size() * sizeof(Material),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  {
    std::vector<Decal> gpuDecals = decals;
    if (gpuDecals.empty()) gpuDecals.push_back({glm::mat4(1.0f), glm::vec4(0.0f), 0xFFFFFFFFu});  // nenhum: marcador
    decalBuffer = rhi::createBufferWithData(ctx, gpuDecals.data(), gpuDecals.size() * sizeof(Decal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  }
  std::vector<InstanceGpu> inst;
  for (auto& o : objects) inst.push_back({meshes[o.mesh].vertices.address, meshes[o.mesh].indices.address, o.material, 0, 0});
  instanceBuffer = rhi::createBufferWithData(ctx, inst.data(), inst.size() * sizeof(InstanceGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  if (ctx.rayTracing) {
    tlas.init(ctx, uint32_t(objects.size()));
    for (uint32_t slot = 0; slot < rhi::DynamicTlas::kSlots; ++slot)
      for (uint32_t i = 0; i < objects.size(); ++i) tlas.writeInstance(slot, i, tlasInstance(i));
    ctx.immediateSubmit([&](VkCommandBuffer cmd) { tlas.build(cmd, 0, uint32_t(objects.size())); });
  }
  size_t drawn = 0;
  for (auto& o : objects) drawn += meshes[o.mesh].indexCount / 3;
  LOG_INFO("Cena: %zu malhas (%zu tri, %zu com esqueleto), %zu objetos (%zu tri desenhados), %zu materiais, "
           "%zu texturas (%.1f Mtexels), %zu luzes, %zu modelos animados",
           meshes.size(), triangles, skinnedCount, objects.size(), drawn, materials.size(), textures.size(), texels / 1e6,
           lights.size(), models.size());
}

void Scene::destroy(rhi::Context& ctx) {
  if (ctx.rayTracing) tlas.destroy();
  for (auto& m : meshes) {
    for (rhi::Buffer* b : {&m.vertices, &m.indices, &m.restVertices, &m.skinData, &m.prevVertices, &m.clothPos, &m.clothPrev})
      rhi::destroyBuffer(ctx, *b);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  for (auto& t : textures) rhi::destroyImage(ctx, t);
  for (rhi::Buffer* b : {&materialBuffer, &instanceBuffer, &skinScratch, &decalBuffer}) rhi::destroyBuffer(ctx, *b);
  meshes.clear();
  textures.clear();
}

}  // namespace scene
