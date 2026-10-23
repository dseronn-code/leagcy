#include "scene/scene.h"

#include <algorithm>
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
  for (auto [node, object] : m.rigidObjects) objects[object].transform = m.root * resolve(node);
  for (uint32_t object : m.skinnedObjects) objects[object].transform = m.root;
  m.jointMatrices.resize(m.skins.size());
  for (size_t s = 0; s < m.skins.size(); ++s) {
    const Skin& skin = m.skins[s];
    m.jointMatrices[s].resize(skin.joints.size());
    for (size_t j = 0; j < skin.joints.size(); ++j) m.jointMatrices[s][j] = resolve(skin.joints[j]) * skin.inverseBind[j];
  }
}

void Scene::update(float dt) {
  time += dt;
  for (auto& o : objects) o.prevTransform = o.transform;
  for (auto& a : animators_) a(*this, time, dt);
  for (auto& m : models) animateModel(m, dt);
}

std::vector<rhi::TlasInstance> Scene::tlasInstances() const {
  std::vector<rhi::TlasInstance> out;
  out.reserve(objects.size());
  for (uint32_t i = 0; i < objects.size(); ++i) {
    bool opaque = !(materials[objects[i].material].flags & kMaterialAlphaMask);
    out.push_back({&meshes[objects[i].mesh].blas, objects[i].transform, i, opaque});
  }
  return out;
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

  VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (ctx.rayTracing) geometryUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  const VkBufferUsageFlags skinnedUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

  size_t triangles = 0, skinnedCount = 0;
  VkDeviceSize scratchTotal = 0;
  for (auto& data : cpuMeshes_) {
    GpuMesh gm;
    bool skinned = !data.joints.empty() && data.skinModel >= 0 && data.skin >= 0;
    VkDeviceSize vbytes = data.vertices.size() * sizeof(Vertex);
    VkBufferUsageFlags vusage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage | (skinned ? skinnedUsage : 0);
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
    if (ctx.rayTracing) {
      rhi::BlasInput in{gm.vertices.address, gm.vertexCount, sizeof(Vertex), gm.indices.address, gm.indexCount};
      gm.blas = rhi::buildBlas(ctx, in, skinned);
      if (skinned) {
        gm.updateScratchOffset = scratchTotal;
        scratchTotal += rhi::blasUpdateScratchSize(ctx, in);
      }
    }
    triangles += data.indices.size() / 3;
    meshes.push_back(gm);
  }
  cpuMeshes_.clear();
  if (scratchTotal > 0)
    skinScratch = rhi::createBuffer(ctx, scratchTotal, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                    rhi::MemoryUsage::GpuOnly);

  size_t texels = 0;
  for (auto& t : cpuTextures_) {
    textures.push_back(rhi::createTexture(ctx, t.rgba.data(), t.width, t.height, t.srgb));
    texels += size_t(t.width) * t.height;
  }
  cpuTextures_.clear();

  if (materials.empty()) addMaterial({});
  materialBuffer = rhi::createBufferWithData(ctx, materials.data(), materials.size() * sizeof(Material),
                                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::vector<InstanceGpu> inst;
  for (auto& o : objects) inst.push_back({meshes[o.mesh].vertices.address, meshes[o.mesh].indices.address, o.material, 0, 0});
  instanceBuffer = rhi::createBufferWithData(ctx, inst.data(), inst.size() * sizeof(InstanceGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  if (ctx.rayTracing) {
    tlas.init(ctx, uint32_t(objects.size()));
    auto instances = tlasInstances();
    ctx.immediateSubmit([&](VkCommandBuffer cmd) { tlas.record(cmd, 0, instances); });
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
    for (rhi::Buffer* b : {&m.vertices, &m.indices, &m.restVertices, &m.skinData, &m.prevVertices}) rhi::destroyBuffer(ctx, *b);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  for (auto& t : textures) rhi::destroyImage(ctx, t);
  for (rhi::Buffer* b : {&materialBuffer, &instanceBuffer, &skinScratch}) rhi::destroyBuffer(ctx, *b);
  meshes.clear();
  textures.clear();
}

}  // namespace scene
