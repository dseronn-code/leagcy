#include "renderer/light_bvh.h"

#include <algorithm>

namespace renderer {

namespace {

float luminance(glm::vec3 c) { return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b; }

struct Builder {
  const std::vector<scene::PointLight>& lights;
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
      n.boundsMin = glm::min(n.boundsMin, l.position - glm::vec3(l.radius));
      n.boundsMax = glm::max(n.boundsMax, l.position + glm::vec3(l.radius));
      n.power += std::max(luminance(l.intensity), 0.0f);
      n.maxRange = std::max(n.maxRange, l.range);
      cmin = glm::min(cmin, l.position);
      cmax = glm::max(cmax, l.position);
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
                     [&](uint32_t a, uint32_t b) { return lights[a].position[axis] < lights[b].position[axis]; });
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
  Builder{lights, order, nodes}.build(0, uint32_t(lights.size()));
}

}  // namespace renderer
