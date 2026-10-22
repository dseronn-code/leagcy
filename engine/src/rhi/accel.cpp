#include "rhi/accel.h"

#include <cstring>

namespace rhi {

namespace {

constexpr VkBufferUsageFlags kAsInputUsage =
    VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

AccelStructure buildAccel(Context& ctx, VkAccelerationStructureTypeKHR type,
                          const VkAccelerationStructureGeometryKHR& geometry, uint32_t primitiveCount) {
  VkAccelerationStructureBuildGeometryInfoKHR build{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
  build.type = type;
  build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
  build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
  build.geometryCount = 1;
  build.pGeometries = &geometry;

  VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
  ctx.rt.vkGetAccelerationStructureBuildSizesKHR(ctx.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build,
                                                 &primitiveCount, &sizes);

  AccelStructure as;
  as.storage = createBuffer(ctx, sizes.accelerationStructureSize,
                            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                            MemoryUsage::GpuOnly);
  VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
  ci.buffer = as.storage.handle;
  ci.size = sizes.accelerationStructureSize;
  ci.type = type;
  VK_CHECK(ctx.rt.vkCreateAccelerationStructureKHR(ctx.device, &ci, nullptr, &as.handle));

  // Scratch precisa respeitar o alinhamento mínimo do dispositivo.
  VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &asProps;
  vkGetPhysicalDeviceProperties2(ctx.gpu, &p2);
  VkDeviceSize align = asProps.minAccelerationStructureScratchOffsetAlignment;
  Buffer scratch = createBuffer(ctx, sizes.buildScratchSize + align,
                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                MemoryUsage::GpuOnly);
  build.dstAccelerationStructure = as.handle;
  build.scratchData.deviceAddress = (scratch.address + align - 1) & ~(align - 1);

  VkAccelerationStructureBuildRangeInfoKHR range{primitiveCount, 0, 0, 0};
  const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    ctx.rt.vkCmdBuildAccelerationStructuresKHR(cmd, 1, &build, &ranges);
  });
  destroyBuffer(ctx, scratch);

  VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
  ai.accelerationStructure = as.handle;
  as.address = ctx.rt.vkGetAccelerationStructureDeviceAddressKHR(ctx.device, &ai);
  return as;
}

}  // namespace

AccelStructure buildBlas(Context& ctx, const BlasInput& in) {
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
  return buildAccel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geom, in.indexCount / 3);
}

AccelStructure buildTlas(Context& ctx, const std::vector<TlasInstance>& instances) {
  std::vector<VkAccelerationStructureInstanceKHR> raw(instances.size());
  for (size_t i = 0; i < instances.size(); ++i) {
    const glm::mat4& m = instances[i].transform;
    auto& r = raw[i];
    // VkTransformMatrixKHR é 3x4 row-major; glm é column-major.
    for (int row = 0; row < 3; ++row)
      for (int col = 0; col < 4; ++col) r.transform.matrix[row][col] = m[col][row];
    r.instanceCustomIndex = instances[i].customIndex;
    r.mask = 0xFF;
    r.instanceShaderBindingTableRecordOffset = 0;
    r.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR |
              (instances[i].opaque ? VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR : VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR);
    r.accelerationStructureReference = instances[i].blas->address;
  }
  Buffer instBuf = createBufferWithData(ctx, raw.data(), raw.size() * sizeof(raw[0]), kAsInputUsage);

  VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
  geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
  geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
  geom.geometry.instances.data.deviceAddress = instBuf.address;
  AccelStructure tlas = buildAccel(ctx, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, geom, uint32_t(raw.size()));
  destroyBuffer(ctx, instBuf);
  return tlas;
}

void destroyAccel(Context& ctx, AccelStructure& as) {
  if (as.handle) ctx.rt.vkDestroyAccelerationStructureKHR(ctx.device, as.handle, nullptr);
  destroyBuffer(ctx, as.storage);
  as = {};
}

}  // namespace rhi
