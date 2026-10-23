#pragma once
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "scene/scene.h"

namespace renderer {

// BVH de luzes para amostragem por importância (Conty Estevez & Kulla, "Importance Sampling of Many
// Lights with Adaptive Tree Splitting", HPG 2018; mesma ideia do PBRT-v4 e do Cycles). Cada nó guarda a
// caixa das esferas das luzes, a potência somada e o maior alcance; a GPU desce a árvore escolhendo o
// filho pela importância estimada no ponto sombreado, e a pdf exata da luz é o produto das escolhas.
// Escala para dezenas de milhares de luzes (log2 N passos por amostra) sem grade fixa no mundo.
// Espelhado em shaders/light_bvh.glsl.
struct LightBvhNode {
  glm::vec3 boundsMin;
  float power;            // soma da luminância das intensidades (cd)
  glm::vec3 boundsMax;
  float maxRange;         // maior alcance entre as luzes do nó (m)
  uint32_t rightOrLight;  // nó interno: índice do filho direito (o esquerdo é o seguinte); folha: luz
  uint32_t leaf;          // 1 = folha (uma luz)
  uint32_t pad0, pad1;
};
static_assert(sizeof(LightBvhNode) == 48);

// Reconstrói a árvore (as luzes podem ter se movido). Nós em pré-ordem; vazio se não há luzes.
void buildLightBvh(const std::vector<scene::PointLight>& lights, std::vector<LightBvhNode>& nodes);

}  // namespace renderer
