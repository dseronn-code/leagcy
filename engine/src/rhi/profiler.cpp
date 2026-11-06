#include "rhi/profiler.h"

#include <cstdio>

namespace rhi {

void GpuProfiler::init(Context& ctx, uint32_t slots) {
  ctx_ = &ctx;
  uint32_t n = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.gpu, &n, nullptr);
  std::vector<VkQueueFamilyProperties> props(n);
  vkGetPhysicalDeviceQueueFamilyProperties(ctx.gpu, &n, props.data());
  enabled_ = props[ctx.queueFamily].timestampValidBits > 0 && ctx.properties.limits.timestampPeriod > 0.0f;
  if (!enabled_) {
    LOG_WARN("GPU sem timestamps: profiler desligado");
    return;
  }
  msPerTick_ = double(ctx.properties.limits.timestampPeriod) * 1e-6;
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = slots * kMaxMarks;
  VK_CHECK(vkCreateQueryPool(ctx.device, &qi, nullptr, &pool_));
  names_.assign(slots, {});
}

void GpuProfiler::destroy() {
  if (pool_) vkDestroyQueryPool(ctx_->device, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
}

void GpuProfiler::beginFrame(VkCommandBuffer cmd, uint32_t slot) {
  if (!enabled_) return;
  slot_ = slot;
  std::vector<const char*>& names = names_[slot];
  uint32_t base = slot * kMaxMarks;
  if (names.size() > 1) {
    std::vector<uint64_t> ticks(names.size());
    VkResult r = vkGetQueryPoolResults(ctx_->device, pool_, base, uint32_t(ticks.size()), ticks.size() * sizeof(uint64_t),
                                       ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r == VK_SUCCESS) {
      for (size_t i = 1; i < ticks.size(); ++i) {
        double ms = double(ticks[i] - ticks[i - 1]) * msPerTick_;
        bool found = false;
        for (auto& [name, total] : totals_)
          if (name == names[i]) { total += ms; found = true; break; }
        if (!found) totals_.push_back({names[i], ms});
      }
      ++frames_;
    }
  }
  names.clear();
  vkCmdResetQueryPool(cmd, pool_, base, kMaxMarks);
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, base);
  names.push_back(nullptr);
}

void GpuProfiler::mark(VkCommandBuffer cmd, const char* name) {
  if (!enabled_) return;
  std::vector<const char*>& names = names_[slot_];
  if (names.size() >= kMaxMarks) return;
  vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool_, slot_ * kMaxMarks + uint32_t(names.size()));
  names.push_back(name);
}

std::string GpuProfiler::report() {
  if (frames_ == 0) return {};
  double sum = 0.0;
  for (auto& [name, total] : totals_) sum += total;
  char buf[96];
  std::snprintf(buf, sizeof(buf), "GPU %.2f ms (%u frames):", sum / frames_, frames_);
  std::string out = buf;
  for (auto& [name, total] : totals_) {
    std::snprintf(buf, sizeof(buf), " %s %.2f |", name, total / frames_);
    out += buf;
  }
  while (!out.empty() && (out.back() == '|' || out.back() == ' ')) out.pop_back();
  totals_.clear();
  frames_ = 0;
  return out;
}

}  // namespace rhi
