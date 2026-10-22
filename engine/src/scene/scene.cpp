#include "scene/scene.h"

namespace scene {

namespace {
// Espelhado em shaders/scene.glsl (InstanceData).
struct InstanceGpu {
  uint64_t vertices, indices;
  uint32_t material, pad0;
  uint64_t pad1;
};
static_assert(sizeof(InstanceGpu) == 32);
}  // namespace

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

void Scene::upload(rhi::Context& ctx) {
  VkBufferUsageFlags geometryUsage = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  if (ctx.rayTracing) geometryUsage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

  size_t triangles = 0;
  for (auto& data : cpuMeshes_) {
    GpuMesh gm;
    gm.vertices = rhi::createBufferWithData(ctx, data.vertices.data(), data.vertices.size() * sizeof(Vertex),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | geometryUsage);
    gm.indices = rhi::createBufferWithData(ctx, data.indices.data(), data.indices.size() * sizeof(uint32_t),
                                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT | geometryUsage);
    gm.indexCount = uint32_t(data.indices.size());
    if (ctx.rayTracing) {
      gm.blas = rhi::buildBlas(ctx, {gm.vertices.address, uint32_t(data.vertices.size()), sizeof(Vertex),
                                     gm.indices.address, gm.indexCount});
    }
    triangles += data.indices.size() / 3;
    meshes.push_back(gm);
  }
  cpuMeshes_.clear();

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
  // Sempre ao menos um elemento: buffers de tamanho zero não são válidos.
  std::vector<PointLight> lightData = lights.empty() ? std::vector<PointLight>{PointLight{}} : lights;
  lightBuffer = rhi::createBufferWithData(ctx, lightData.data(), lightData.size() * sizeof(PointLight),
                                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

  if (ctx.rayTracing) {
    std::vector<rhi::TlasInstance> instances;
    for (uint32_t i = 0; i < objects.size(); ++i) {
      bool opaque = !(materials[objects[i].material].flags & kMaterialAlphaMask);
      instances.push_back({&meshes[objects[i].mesh].blas, objects[i].transform, i, opaque});
    }
    tlas = rhi::buildTlas(ctx, instances);
  }
  size_t drawn = 0;
  for (auto& o : objects) drawn += meshes[o.mesh].indexCount / 3;
  LOG_INFO("Cena: %zu malhas (%zu tri), %zu objetos (%zu tri desenhados), %zu materiais, %zu texturas (%.1f Mtexels), %zu luzes",
           meshes.size(), triangles, objects.size(), drawn, materials.size(), textures.size(), texels / 1e6, lights.size());
}

void Scene::destroy(rhi::Context& ctx) {
  if (ctx.rayTracing) rhi::destroyAccel(ctx, tlas);
  for (auto& m : meshes) {
    rhi::destroyBuffer(ctx, m.vertices);
    rhi::destroyBuffer(ctx, m.indices);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  for (auto& t : textures) rhi::destroyImage(ctx, t);
  rhi::destroyBuffer(ctx, materialBuffer);
  rhi::destroyBuffer(ctx, instanceBuffer);
  rhi::destroyBuffer(ctx, lightBuffer);
  meshes.clear();
  textures.clear();
}

}  // namespace scene
