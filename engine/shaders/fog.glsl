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

// Direção com densidade henyeyGreenstein(dot(v, w), g) (r uniforme em [0,1)²): a luz que chega por w espalha em v
// com a fase da névoa. Amostrar o céu assim dá ∫ céu × fase sem peso (a pdf é a própria fase).
vec3 sampleFogPhase(vec3 v, float g, vec2 r) {
  float s = (1.0 - g * g) / (1.0 - g + 2.0 * g * r.x);
  float c = clamp((1.0 + g * g - s * s) / (2.0 * g), -1.0, 1.0);
  float sn = sqrt(max(1.0 - c * c, 0.0)), ph = TWO_PI * r.y;
  vec3 a = abs(v.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
  vec3 x = normalize(cross(a, v)), y = cross(v, x);
  return normalize(v * c + (x * cos(ph) + y * sin(ph)) * sn);
}

// Céu espalhado pela névoa na direção v, sem sombra: ∫ céu(w) × fase(v·w) dw por 16 direções fixas (estratificadas
// pela fase em volta de v; sem ruído). Abaixo do horizonte vale o que a LUT do céu dá (a luz do ar até o chão), como
// nos raios de visibilidade perto da câmera que não acertam nada.
vec3 fogSkyAmbient(vec3 v) {
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 16; ++i) sum += sampleSky(sampleFogPhase(v, FOG_PHASE_G, (vec2(i & 3, i >> 2) + 0.5) * 0.25));
  return sum * (1.0 / 16.0);
}

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
  vec3 ambient = fogSkyAmbient(v);
  L = (sunLight + ambient) * (1.0 - T);
}

#endif
