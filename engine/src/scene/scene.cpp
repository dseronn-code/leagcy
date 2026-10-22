#include "scene/scene.h"

namespace scene {

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

  if (ctx.rayTracing) {
    std::vector<rhi::TlasInstance> instances;
    for (uint32_t i = 0; i < objects.size(); ++i)
      instances.push_back({&meshes[objects[i].mesh].blas, objects[i].transform, i});
    tlas = rhi::buildTlas(ctx, instances);
  }
  size_t instanced = 0;
  for (auto& o : objects) instanced += meshes[o.mesh].indexCount / 3;
  LOG_INFO("Cena: %zu malhas (%zu tri únicos), %zu objetos (%zu tri desenhados)", meshes.size(), triangles,
           objects.size(), instanced);
}

void Scene::destroy(rhi::Context& ctx) {
  if (ctx.rayTracing) rhi::destroyAccel(ctx, tlas);
  for (auto& m : meshes) {
    rhi::destroyBuffer(ctx, m.vertices);
    rhi::destroyBuffer(ctx, m.indices);
    if (ctx.rayTracing) rhi::destroyAccel(ctx, m.blas);
  }
  meshes.clear();
}

}  // namespace scene
