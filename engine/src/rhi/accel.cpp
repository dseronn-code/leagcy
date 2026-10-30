#include "rhi/accel.h"

#include <algorithm>
#include <cstring>

namespace rhi {

namespace {

constexpr VkBuildAccelerationStructureFlagsKHR kStaticFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
constexpr VkBuildAccelerationStructureFlagsKHR kUpdatableFlags =
    VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
// TLAS muda todo frame: construção rápida vale mais que traçado marginalmente melhor.
constexpr VkBuildAccelerationStructureFlagsKHR kTlasFlags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;

VkDeviceSize scratchAlignment(Context& ctx) {
  VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &asProps;
  vkGetPhysicalDeviceProperties2(ctx.gpu, &p2);
  return asProps.minAccelerationStructureScratchOffsetAlignment;
}

VkAccelerationStructureGeometryKHR triangleGeometry(const BlasInput& in) {
  VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
  geom.flags = 0;  // opacidade decidida por instância (FORCE_OPAQUE / FORCE_NO_OPAQUE)
  auto& tri = geom.geometry.triangles;
  tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
  tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
  tri.vertexData.deviceAddress = in.vertexAddress;
  tri.vertexStride = in.vertexStride;
  tri.maxVertex = in.vertexCount - 1;
  tri.indexType = VK_INDEX_TYPE_UINT32;
  tri.indexData.deviceAddress = in.indexAddress;
  return geom;
}

VkAccelerationStructureGeometryKHR instanceGeometry(VkDeviceAddress instances) {
  VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geom.geometry.instances.data.deviceAddress = instances;
  return geom;
}

VkAccelerationStructureBuildSizesInfoKHR querySizes(Context& ctx, VkAccelerationStructureBuildGeometryInfoKHR& build,
                                                   uint32_t primitiveCount) {
  VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  ctx.rt.vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                                 &primitiveCount, &sizes);
  return sizes;
}

AccelStructure createAccel(Context& ctx, VkAccelerationStructureTypeKHR type, VkDeviceSize size) {
  AccelStructure as;
  as.storage = createBuffer(ctx, size,
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            MemoryUsage::GpuOnly);
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = as.storage.handle;
  ci.size = size;
  ci.type = type;
  VK_CHECK(ctx.rt.vkCreateAccelerationStructureKHR(ctx.device, &ci, nullptr, &as.handle));
  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  ai.accelerationStructure = as.handle;
  as.address = ctx.rt.vkGetAccelerationStructureDeviceAddressKHR(ctx.device, &ai);
  return as;
}

Buffer createScratch(Context& ctx, VkDeviceSize size) {
  return createBuffer(ctx, size + scratchAlignment(ctx),
                      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, MemoryUsage::GpuOnly);
}

}  // namespace

VkDeviceAddress alignScratch(Context& ctx, VkDeviceAddress address) {
  VkDeviceSize a = scratchAlignment(ctx);
  return (address + a - 1) & ~(a - 1);
}

AccelStructure buildBlas(Context& ctx, const BlasInput& in, bool allowUpdate) {
  VkAccelerationStructureGeometryKHR geom = triangleGeometry(in);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = allowUpdate ? kUpdatableFlags : kStaticFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  uint32_t primitives = in.indexCount / 3;
  auto sizes = querySizes(ctx, build, primitives);

  AccelStructure as = createAccel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
  Buffer scratch = createScratch(ctx, sizes.buildScratchSize);
  build.dstAccelerationStructure = as.handle;
  build.scratchData.deviceAddress = alignScratch(ctx, scratch.address);
  VkAccelerationStructureBuildRangeInfoKHR range{primitives, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  ctx.immediateSubmit([&](VkCommandBuffer cmd) { ctx.rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges); });
  destroyBuffer(ctx, scratch);
  return as;
}

VkDeviceSize blasUpdateScratchSize(Context& ctx, const BlasInput& in) {
  VkAccelerationStructureGeometryKHR geom = triangleGeometry(in);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = kUpdatableFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  return querySizes(ctx, build, in.indexCount / 3).updateScratchSize + scratchAlignment(ctx);
}

void recordBlasUpdate(Context& ctx, VkCommandBuffer cmd, AccelStructure& blas, const BlasInput& in, VkDeviceAddress scratch) {
  VkAccelerationStructureGeometryKHR geom = triangleGeometry(in);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
  build.flags = kUpdatableFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
  build.srcAccelerationStructure = blas.handle;
  build.dstAccelerationStructure = blas.handle;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  build.scratchData.deviceAddress = scratch;
  VkAccelerationStructureBuildRangeInfoKHR range{in.indexCount / 3, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  ctx.rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
}

void destroyAccel(Context& ctx, AccelStructure& as) {
  if (as.handle) ctx.rt.vkDestroyAccelerationStructureKHR(ctx.device, as.handle, nullptr);
  destroyBuffer(ctx, as.storage);
  as = {};
}

void DynamicTlas::init(Context& ctx, uint32_t maxInstances) {
  ctx_ = &ctx;
  maxInstances_ = std::max(1u, maxInstances);
  for (auto& b : instanceBuffers_)
    b = createBuffer(ctx, VkDeviceSize(maxInstances_) * sizeof(VkAccelerationStructureInstanceKHR),
                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                     MemoryUsage::CpuToGpu);
  VkAccelerationStructureGeometryKHR geom = instanceGeometry(instanceBuffers_[0].address);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = kTlasFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  auto sizes = querySizes(ctx, build, maxInstances_);
  tlas = createAccel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
  scratch_ = createScratch(ctx, sizes.buildScratchSize);
}

void DynamicTlas::writeInstance(uint32_t slot, uint32_t index, const TlasInstance& instance) {
  if (index >= maxInstances_) FATAL("TLAS: instância %u (máximo %u)", index, maxInstances_);
  const glm::mat4& m = instance.transform;
  VkAccelerationStructureInstanceKHR r{};
  // VkTransformMatrixKHR é 3x4 row-major; glm é column-major.
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 4; ++col) r.transform.matrix[row][col] = m[col][row];
  r.instanceCustomIndex = instance.customIndex;
  r.mask = instance.mask;
  r.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
            (instance.opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
  // Sem BLAS (malha fora da memória: streaming): instância inativa — endereço 0, nunca atingida.
  r.accelerationStructureReference = instance.blas ? instance.blas->address : 0;
  std::memcpy(static_cast<VkAccelerationStructureInstanceKHR*>(instanceBuffers_[slot % kSlots].mapped) + index, &r, sizeof(r));
}

void DynamicTlas::build(VkCommandBuffer cmd, uint32_t slot, uint32_t count) {
  Buffer& buf = instanceBuffers_[slot % kSlots];
  vmaFlushAllocation(ctx_->allocator, buf.allocation, 0, VK_WHOLE_SIZE);

  VkAccelerationStructureGeometryKHR geom = instanceGeometry(buf.address);
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
  build.flags = kTlasFlags;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.dstAccelerationStructure = tlas.handle;
  build.geometryCount = 1;
  build.pGeometries = &geom;
  build.scratchData.deviceAddress = alignScratch(*ctx_, scratch_.address);
  VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  ctx_->rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
}

void DynamicTlas::destroy() {
  if (!ctx_) return;
  destroyAccel(*ctx_, tlas);
  for (auto& b : instanceBuffers_) destroyBuffer(*ctx_, b);
  destroyBuffer(*ctx_, scratch_);
  ctx_ = nullptr;
}

}  // namespace rhi
