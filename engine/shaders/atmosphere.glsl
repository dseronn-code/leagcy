// Atmosfera física (Hillaire, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique",
// EGSR 2020): Rayleigh + Mie + ozônio sobre um planeta esférico, unidades em km.
// Este arquivo tem só a física e as parametrizações; as LUTs ficam em atmosphere_lut.glsl.
#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

#include "common.glsl"

const float R_GROUND = 6360.0;
const float R_TOP = 6460.0;
const vec3 RAYLEIGH_SCATTER = vec3(5.802, 13.558, 33.1) * 1e-3;
const float RAYLEIGH_H = 8.0;
const float MIE_SCATTER = 3.996e-3;
const float MIE_EXTINCT = 4.40e-3;
const float MIE_H = 1.2;
const float MIE_G = 0.8;
const vec3 OZONE_ABSORB = vec3(0.650, 1.881, 0.085) * 1e-3;
const vec3 GROUND_ALBEDO = vec3(0.3);

// Distância até a esfera de raio r centrada na origem; -1 se não houver interseção à frente.
float raySphere(vec3 ro, vec3 rd, float r) {
  float b = dot(ro, rd);
  float c = dot(ro, ro) - r * r;
  float disc = b * b - c;
  if (disc < 0.0) return -1.0;
  disc = sqrt(disc);
  float t0 = -b - disc, t1 = -b + disc;
  if (t0 > 0.0) return t0;
  if (t1 > 0.0) return t1;
  return -1.0;
}

void sampleMedium(float h, out vec3 scatR, out float scatM, out vec3 extinction) {
  float dR = exp(-h / RAYLEIGH_H);
  float dM = exp(-h / MIE_H);
  float dO = max(0.0, 1.0 - abs(h - 25.0) / 15.0);
  scatR = RAYLEIGH_SCATTER * dR;
  scatM = MIE_SCATTER * dM;
  extinction = scatR + MIE_EXTINCT * dM + OZONE_ABSORB * dO;
}

float rayleighPhase(float mu) { return 3.0 / (16.0 * PI) * (1.0 + mu * mu); }
float henyeyGreenstein(float mu, float g) {
  float d = 1.0 + g * g - 2.0 * g * mu;
  return (1.0 - g * g) / (4.0 * PI * d * sqrt(d));
}
float miePhase(float mu) {  // Cornette-Shanks
  const float g = MIE_G;
  float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
  return k * (1.0 + mu * mu) / pow(1.0 + g * g - 2.0 * g * mu, 1.5);
}
const float ISOTROPIC_PHASE = 1.0 / (4.0 * PI);

// ---- Parametrização da LUT de transmitância (Bruneton & Neyret 2008) ----
// u: distância até o topo normalizada entre o mínimo e o máximo possível; v: altitude em "rho".
const float ATMO_H = 1132.2544;  // sqrt(R_TOP² - R_GROUND²)

vec2 transmittanceUvFromRMu(float r, float mu) {
  float rho = sqrt(max(r * r - R_GROUND * R_GROUND, 0.0));
  float disc = r * r * (mu * mu - 1.0) + R_TOP * R_TOP;
  float d = max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
  float dMin = R_TOP - r, dMax = rho + ATMO_H;
  return vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / ATMO_H);
}
void rMuFromTransmittanceUv(vec2 uv, out float r, out float mu) {
  float rho = ATMO_H * uv.y;
  r = sqrt(rho * rho + R_GROUND * R_GROUND);
  float dMin = R_TOP - r, dMax = rho + ATMO_H;
  float d = dMin + uv.x * (dMax - dMin);
  mu = d == 0.0 ? 1.0 : clamp((ATMO_H * ATMO_H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

// Transmitância de um ponto (altitude r, cosseno zenital mu) até o topo, por raymarch (referência).
vec3 transmittanceRaymarch(float r, float mu, int steps) {
  vec3 ro = vec3(0.0, r, 0.0);
  vec3 rd = vec3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);
  float tMax = raySphere(ro, rd, R_TOP);
  if (tMax <= 0.0) return vec3(1.0);
  float dt = tMax / float(steps);
  vec3 od = vec3(0.0);
  for (int i = 0; i < steps; ++i) {
    vec3 q = ro + rd * ((float(i) + 0.5) * dt);
    vec3 sR, ext; float sM;
    sampleMedium(length(q) - R_GROUND, sR, sM, ext);
    od += ext * dt;
  }
  return exp(-od);
}

// ---- Parametrização da LUT de múltiplo espalhamento: (cos zenital do sol, altitude) ----
vec2 multiScatterUv(float cosSunZenith, float h) {
  return vec2(cosSunZenith * 0.5 + 0.5, clamp(h / (R_TOP - R_GROUND), 0.0, 1.0));
}

#endif
