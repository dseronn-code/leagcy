// Névoa volumétrica em "froxels" (células da grade 3D presa ao frustum; Wronski 2014, Hillaire 2015): a luz do sol
// entra em cada célula com a sombra de verdade (raio até o sol), então aparecem os raios de luz ("god rays") entre
// prédios e árvores e a névoa dentro da sombra fica escura. Até FOG_FAR metros; além, a névoa analítica de altura.
#ifndef FOG_GLSL
#define FOG_GLSL

#include "frame.glsl"

const float FOG_NEAR = 0.3, FOG_FAR = 200.0;  // m (distância ao longo do raio da câmera)
const float FOG_PHASE_G = 0.6;                // espalhamento para a frente (aerossol)

// Fatias exponenciais: resolução perto da câmera, onde os detalhes aparecem.
float fogSliceDistance(float w) { return FOG_NEAR * pow(FOG_FAR / FOG_NEAR, w); }  // w = fatia / nº de fatias
float fogSliceCoord(float dist) { return log(max(dist, FOG_NEAR) / FOG_NEAR) / log(FOG_FAR / FOG_NEAR); }

// Densidade de extinção da névoa de altura (1/m).
float fogDensityAt(vec3 p) { return frame.params.z * exp(-frame.params.w * p.y); }

// Trecho analítico [t0, t1] do raio (o + v t) na névoa de altura, sem sombra: transmitância e luz espalhada.
void heightFogSegment(vec3 o, vec3 v, float t0, float t1, out float T, out vec3 L) {
  T = 1.0;
  L = vec3(0.0);
  if (frame.params.z <= 0.0 || t1 <= t0) return;
  float density = frame.params.z, falloff = frame.params.w;
  vec3 p0 = o + v * t0;
  float len = t1 - t0, k = falloff * v.y * len;
  float lineIntegral = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
  T = exp(-density * exp(-falloff * p0.y) * lineIntegral * len);
  vec3 sunLight = sunIrradianceAt(p0) * henyeyGreenstein(dot(v, frame.sunDir.xyz), FOG_PHASE_G);
  vec3 ambient = sampleSky(normalize(vec3(v.x, 0.15, v.z)));
  L = (sunLight + ambient) * (1.0 - T);
}

#endif
