#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

namespace game {

// Tamanho do agente (NPC) para o qual a malha é gerada.
struct NavSettings {
  float cellSize = 0.15f, cellHeight = 0.1f;  // resolução da voxelização (m)
  float agentHeight = 1.8f, agentRadius = 0.3f, agentClimb = 0.4f, agentMaxSlopeDeg = 50.0f;
};

// Malha de navegação sobre o Recast/Detour: a partir dos triângulos parados do cenário, calcula onde um agente de
// tamanho NavSettings pode andar (respeitando degraus, rampas e o espaço em volta das paredes) e acha caminhos.
class Navigation {
 public:
  Navigation();
  ~Navigation();
  bool build(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices, const NavSettings& settings);
  // Caminho em linha reta entre os cantos (pontos no chão), de start até end (ou até o ponto alcançável mais perto).
  bool findPath(glm::vec3 start, glm::vec3 end, std::vector<glm::vec3>& path) const;
  bool ok() const;
  int polygonCount() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
