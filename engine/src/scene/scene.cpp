#include "scene/scene.h"

#include <chrono>

#include "scene/texture_compress.h"

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
  bool opaque = !(materials[objects[i].material].flags & kMaterialAlphaMask);
  return {&meshes[objects[i].mesh].blas, objects[i].transform, i, opaque};
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
  VkFormatProperties bc7{};
  vkGetPhysicalDeviceFormatProperties(ctx.gpu, VK_FORMAT_BC7_SRGB_BLOCK, &bc7);
  const bool compress = compressTextures && !cpuTextures_.empty() &&
                        (bc7.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  if (compressTextures && !compress && !cpuTextures_.empty()) LOG_INFO("Texturas: GPU sem BC7, usando RGBA8");
  double bytesRaw = 0.0, bytesGpu = 0.0, psnrSum = 0.0, psnrMin = 99.0;
  uint32_t encoded = 0, precompressed = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (auto& t : cpuTextures_) {
    const double raw = double(t.width) * t.height * 4.0 * 4.0 / 3.0;  // RGBA8 com a cadeia de mips
    bytesRaw += raw;
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
      CompressedTexture ct = compressTexture(t.rgba.data(), t.width, t.height, t.srgb, t.normalMap, textureCacheDir);
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
    for (rhi::Buffer* b : {&m.vertices, &m.indices, &m.restVertices, &m.skinData, &m.prevVertices}) rhi::destroyBuffer(ctx, *b);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  for (auto& t : textures) rhi::destroyImage(ctx, t);
  for (rhi::Buffer* b : {&materialBuffer, &instanceBuffer, &skinScratch}) rhi::destroyBuffer(ctx, *b);
  meshes.clear();
  textures.clear();
}

}  // namespace scene
