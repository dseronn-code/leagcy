// Consultas às LUTs da atmosfera (set 0: 10 = transmitância, 11 = múltiplo espalhamento,
// 12 = perspectiva aérea) e integração do céu. Incluído por frame.glsl.
#ifndef ATMOSPHERE_LUT_GLSL
#define ATMOSPHERE_LUT_GLSL

#include "atmosphere.glsl"

layout(set = 0, binding = 10) uniform sampler2D transmittanceLut;
layout(set = 0, binding = 11) uniform sampler2D multiScatterLut;
layout(set = 0, binding = 12) uniform sampler3D aerialPerspectiveLut;

// Transmitância de p (km, centro do planeta na origem) até o topo na direção dir; zero abaixo do
// horizonte planetário (sombra da Terra), com transição suave para não serrilhar o crepúsculo.
vec3 transmittanceToTop(vec3 p, vec3 dir) {
  float r = length(p);
  float mu = dot(p / r, dir);
  float muHorizon = -sqrt(max(0.0, 1.0 - (R_GROUND * R_GROUND) / (r * r)));
  float fade = smoothstep(muHorizon - 0.004, muHorizon + 0.004, mu);
  if (fade <= 0.0) return vec3(0.0);
  return textureLod(transmittanceLut, transmittanceUvFromRMu(r, max(mu, muHorizon)), 0.0).rgb * fade;
}
vec3 transmittanceToSun(vec3 p, vec3 sunDir) { return transmittanceToTop(p, sunDir); }

// Luminância de múltiplo espalhamento por unidade de iluminância solar e de coeficiente de espalhamento (Ψms).
vec3 multiScatter(vec3 p, vec3 sunDir) {
  float r = length(p);
  return textureLod(multiScatterLut, multiScatterUv(dot(p / r, sunDir), r - R_GROUND), 0.0).rgb;
}

// In-scattering e transmitância ao longo de [0, tMax] de um raio, com espalhamento simples (fases
// Rayleigh/Mie) + múltiplo espalhamento (LUT). Integração com conservação de energia por segmento.
void integrateAtmosphere(vec3 ro, vec3 rd, float tMax, vec3 sunDir, float sunE, int steps,
                         out vec3 L, out vec3 T) {
  float mu = dot(rd, sunDir);
  float pR = rayleighPhase(mu), pM = miePhase(mu);
  L = vec3(0.0);
  T = vec3(1.0);
  float tPrev = 0.0;
  for (int i = 0; i < steps; ++i) {
    // Passos quadráticos: mais amostras perto do observador, onde o ar é denso.
    float f = (float(i) + 1.0) / float(steps);
    float t = tMax * f * f;
    float dt = t - tPrev;
    vec3 p = ro + rd * (tPrev + 0.5 * dt);
    tPrev = t;
    vec3 sR, ext; float sM;
    sampleMedium(length(p) - R_GROUND, sR, sM, ext);
    vec3 S = sunE * (transmittanceToSun(p, sunDir) * (sR * pR + sM * pM) + multiScatter(p, sunDir) * (sR + sM));
    vec3 sampleT = exp(-ext * dt);
    L += T * (S - S * sampleT) / max(ext, vec3(1e-7));
    T *= sampleT;
  }
}

// Radiância do céu numa direção (até o topo ou até o chão), por unidade de pré-exposição.
vec3 integrateSky(vec3 ro, vec3 rd, vec3 sunDir, float sunE) {
  float tTop = raySphere(ro, rd, R_TOP);
  if (tTop < 0.0) return vec3(0.0);
  float tGround = raySphere(ro, rd, R_GROUND);
  float tMax = tGround > 0.0 ? tGround : tTop;
  vec3 L, T;
  integrateAtmosphere(ro, rd, tMax, sunDir, sunE, 96, L, T);
  if (tGround > 0.0) {  // chão ao longe (abaixo do horizonte): Lambert iluminado pelo sol
    vec3 pg = ro + rd * tGround;
    vec3 n = normalize(pg);
    L += T * transmittanceToSun(pg, sunDir) * sunE * max(dot(n, sunDir), 0.0) * GROUND_ALBEDO / PI;
  }
  return L;
}

#endif
