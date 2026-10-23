// ReSTIR DI (Bitterli et al., SIGGRAPH 2020): reamostragem espaço-temporal de importância
// para iluminação direta com muitas luzes. Cada pixel guarda um "reservatório" com a luz escolhida.
#ifndef RESTIR_GLSL
#define RESTIR_GLSL

#include "brdf.glsl"
#include "gbuffer.glsl"
#include "scene.glsl"

struct Reservoir {
  uint light;   // luz escolhida (0xFFFFFFFF = nenhuma)
  float wSum;   // soma dos pesos de reamostragem
  float M;      // quantas amostras o reservatório representa
  float W;      // peso de contribuição não enviesado
};

const uint NO_LIGHT = 0xFFFFFFFFu;
uint restirCandidates() { return max(frame.quality0.y, 1u); }  // candidatos iniciais por pixel (preset)
const float RESTIR_MAX_HISTORY = 20.0;  // limite de M temporal (múltiplo dos candidatos iniciais)

Reservoir emptyReservoir() { return Reservoir(NO_LIGHT, 0.0, 0.0, 0.0); }

bool updateReservoir(inout Reservoir r, uint light, float weight, float M, float u) {
  r.wSum += weight;
  r.M += M;
  if (weight > 0.0 && u * r.wSum <= weight) {
    r.light = light;
    return true;
  }
  return false;
}

BrdfContext brdfContextFor(GSample g) {
  return makeBrdfContext(g.N, g.V, g.f0, g.f90, g.roughness, energyCompensationEnabled());
}

// Função alvo p̂: luminância da contribuição sem sombra, avaliada no ponto do pixel.
float targetPdf(uint li, GSample g, BrdfContext bc) {
  if (li == NO_LIGHT) return 0.0;
  PointLight l = lights[li];
  vec3 Lv = l.position - g.P;
  float dist = length(Lv);
  if (dist >= l.range) return 0.0;
  vec3 L = Lv / dist;
  vec3 kd, ks;
  evalBRDF(bc, L, -1.0, kd, ks);
  return luminance((kd * g.diffuseColor + ks) * lightIlluminanceApprox(l, dist));
}

void finalizeReservoir(inout Reservoir r, GSample g, BrdfContext bc) {
  float p = targetPdf(r.light, g, bc);
  r.W = p > 0.0 ? r.wSum / (r.M * p) : 0.0;
}

#endif
