// Atmosfera física: Rayleigh + Mie + ozônio, unidades em km (modelo de Hillaire, EGSR 2020).
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
const vec3 OZONE_ABSORB = vec3(0.650, 1.881, 0.085) * 1e-3;

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
  const float g = 0.8;
  float k = 3.0 / (8.0 * PI) * (1.0 - g * g) / (2.0 + g * g);
  return k * (1.0 + mu * mu) / pow(1.0 + g * g - 2.0 * g * mu, 1.5);
}

vec3 transmittanceToSun(vec3 p, vec3 sunDir) {
  // Sombra suave do horizonte planetário em vez de corte seco.
  float cosHorizon = -sqrt(max(0.0, 1.0 - (R_GROUND * R_GROUND) / dot(p, p)));
  float horizonFade = smoothstep(cosHorizon - 0.01, cosHorizon + 0.01, dot(normalize(p), sunDir));
  float tMax = raySphere(p, sunDir, R_TOP);
  if (tMax <= 0.0 || horizonFade <= 0.0) return vec3(0.0);
  const int N = 16;
  float dt = tMax / float(N);
  vec3 od = vec3(0.0);
  for (int i = 0; i < N; ++i) {
    vec3 q = p + sunDir * ((float(i) + 0.5) * dt);
    vec3 sR, ext; float sM;
    sampleMedium(length(q) - R_GROUND, sR, sM, ext);
    od += ext * dt;
  }
  return exp(-od) * horizonFade;
}

// Radiância do céu ao longo de um raio (single scattering + aproximação de múltiplo espalhamento).
vec3 integrateSky(vec3 ro, vec3 rd, vec3 sunDir, float sunE) {
  float tTop = raySphere(ro, rd, R_TOP);
  if (tTop < 0.0) return vec3(0.0);
  float tGround = raySphere(ro, rd, R_GROUND);
  float tMax = tGround > 0.0 ? tGround : tTop;

  float mu = dot(rd, sunDir);
  float pR = rayleighPhase(mu), pM = miePhase(mu);
  const int N = 40;
  vec3 L = vec3(0.0), T = vec3(1.0);
  float tPrev = 0.0;
  for (int i = 0; i < N; ++i) {
    // Passos quadráticos: mais amostras perto do observador, onde o ar é denso.
    float f = (float(i) + 1.0) / float(N);
    float t = tMax * f * f;
    float dt = t - tPrev;
    vec3 p = ro + rd * (tPrev + 0.5 * dt);
    tPrev = t;
    vec3 sR, ext; float sM;
    sampleMedium(length(p) - R_GROUND, sR, sM, ext);
    vec3 Tsun = transmittanceToSun(p, sunDir);
    // Múltiplo espalhamento aproximado como termo isotrópico proporcional ao espalhamento.
    vec3 ms = (sR + sM) * 0.08 * (0.3 + 0.7 * Tsun);
    vec3 S = sunE * (Tsun * (sR * pR + sM * pM) + ms);
    vec3 sampleT = exp(-ext * dt);
    L += T * (S - S * sampleT) / max(ext, vec3(1e-7));  // integração com conservação de energia
    T *= sampleT;
  }
  return L;
}

#endif
