#pragma once
#include <string>
#include <utility>
#include <vector>

#include "rhi/context.h"

namespace rhi {

// Tempo de GPU por passe via timestamps. Cada frame em voo tem sua faixa de consultas; os resultados de
// um slot são lidos quando ele é reutilizado (a cerca do frame já garantiu que a GPU terminou).
class GpuProfiler {
 public:
  static constexpr uint32_t kMaxMarks = 32;

  void init(Context& ctx, uint32_t slots);
  void destroy();
  void beginFrame(VkCommandBuffer cmd, uint32_t slot);
  // Fecha o intervalo desde a marca anterior com este nome (string estática).
  void mark(VkCommandBuffer cmd, const char* name);
  // Médias por passe desde o último relatório (e zera o acumulado). Vazio se não houver amostras.
  std::string report();

 private:
  Context* ctx_ = nullptr;
  VkQueryPool pool_ = VK_NULL_HANDLE;
  bool enabled_ = false;
  double msPerTick_ = 0.0;
  uint32_t slot_ = 0;
  std::vector<std::vector<const char*>> names_;  // por slot: rótulo de cada intervalo
  std::vector<std::pair<const char*, double>> totals_;  // ordem de aparição
  uint32_t frames_ = 0;
};

}  // namespace rhi
