#include "renderer/light_bvh.h"

#include <algorithm>

namespace renderer {

namespace {

struct Builder {
  const std::vector<scene::PointLight>& lights;
  const std::vector<glm::vec3>& centers;
  std::vector<uint32_t>& order;
  std::vector<LightBvhNode>& nodes;

  // Constrói o nó para order[begin, end) e devolve o índice dele.
  uint32_t build(uint32_t begin, uint32_t end) {
    uint32_t index = uint32_t(nodes.size());
    nodes.emplace_back();
    LightBvhNode n{};
    n.boundsMin = glm::vec3(1e30f);
    n.boundsMax = glm::vec3(-1e30f);
    glm::vec3 cmin(1e30f), cmax(-1e30f);
    for (uint32_t i = begin; i < end; ++i) {
      const scene::PointLight& l = lights[order[i]];
      glm::vec3 lo, hi;
      scene::lightBounds(l, lo, hi);
      n.boundsMin = glm::min(n.boundsMin, lo);
      n.boundsMax = glm::max(n.boundsMax, hi);
      n.power += scene::lightPower(l);
      n.maxRange = std::max(n.maxRange, l.range);
      cmin = glm::min(cmin, centers[order[i]]);
      cmax = glm::max(cmax, centers[order[i]]);
    }
    if (end - begin == 1) {
      n.leaf = 1;
      n.rightOrLight = order[begin];
      nodes[index] = n;
      return index;
    }
    // Divide pela mediana no eixo mais longo dos centros: árvore equilibrada (profundidade log2 N).
    glm::vec3 ext = cmax - cmin;
    int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : (ext.y >= ext.z ? 1 : 2);
    uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(order.begin() + begin, order.begin() + mid, order.begin() + end,
                     [&](uint32_t a, uint32_t b) { return centers[a][axis] < centers[b][axis]; });
    build(begin, mid);
    n.rightOrLight = build(mid, end);
    nodes[index] = n;
    return index;
  }
};

}  // namespace

void buildLightBvh(const std::vector<scene::PointLight>& lights, std::vector<LightBvhNode>& nodes) {
  nodes.clear();
  if (lights.empty()) return;
  std::vector<uint32_t> order(lights.size());
  for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
  nodes.reserve(2 * lights.size());
  std::vector<glm::vec3> centers(lights.size());
  for (size_t i = 0; i < lights.size(); ++i) centers[i] = scene::lightCenter(lights[i]);
  Builder{lights, centers, order, nodes}.build(0, uint32_t(lights.size()));
}

}  // namespace renderer
