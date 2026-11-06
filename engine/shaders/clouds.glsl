// Nuvens volumétricas (passes clouds_sky.comp e cloud_shadow.comp): camada de cúmulos entre 1,5 e 4 km de altitude
// sobre o planeta, com a forma do "Nubis" (Schneider 2015/2022: Perlin-Worley + erosão por Worley, perfil de altura
// arredondado no topo) e a luz de Hillaire 2016 (integração que conserva energia, aproximação de múltiplo
// espalhamento por oitavas de Wrenninge, fase de duas lobas). Unidades: km, como a atmosfera.
#ifndef CLOUDS_GLSL
#define CLOUDS_GLSL

#include "frame.glsl"

layout(set = 0, binding = 15) uniform sampler3D cloudNoise;

const float CLOUD_BOTTOM = 1.5, CLOUD_TOP = 4.0;  // km acima do chão
const float CLOUD_EXTINCTION = 40.0;              // 1/km na densidade 1 (cúmulo denso: ~0,05/m)
const float CLOUD_ALBEDO = 0.99;                  // gotas de água quase não absorvem
// Ajustados contra a referência por Monte Carlo (clouds_reference.comp, tools/nuvens.py).
#ifndef CLOUD_MS_A
#define CLOUD_MS_A 0.6
#define CLOUD_MS_B 0.33
#define CLOUD_MS_C 0.5
#define CLOUD_AMB 1.0
#define CLOUD_AMB_ATT 1.0
#endif

float cloudCoverage() { return frame.night.z; }

float remap(float x, float a, float b, float c, float d) { return c + (x - a) / max(b - a, 1e-5) * (d - c); }

// Ponto do planeta (km, centro na origem) de uma posição do mundo (m): a posição horizontal conta para as nuvens.
vec3 cloudPlanetPos(vec3 worldMeters) {
  return vec3(worldMeters.x * 0.001, R_GROUND + max(worldMeters.y * 0.001, 0.0) + 0.002, worldMeters.z * 0.001);
}

// Densidade (0..1) num ponto do planeta. `h` = fração da altura dentro da camada.
float cloudDensity(vec3 p, float h, bool detail) {
  if (h <= 0.0 || h >= 1.0) return 0.0;
  vec3 q = p + vec3(1.0, 0.0, 0.35) * (frame.night.w * 0.012);  // vento de ~12 m/s
  // Clima: onde há nuvem (coverage local), por um corte em baixa frequência do mesmo ruído.
  float weather = textureLod(cloudNoise, vec3(q.xz * 0.011, 0.37), 0.0).g;
  float coverage = clamp(cloudCoverage() + (weather - 0.5) * 0.6, 0.0, 1.0);
  vec4 n = textureLod(cloudNoise, q * 0.09, 0.0);
  float fbm = n.g * 0.625 + n.b * 0.25 + n.a * 0.125;
  float shape = remap(n.r, fbm - 1.0, 1.0, 0.0, 1.0);
  // Perfil do cúmulo: base reta, topo arredondado.
  shape *= smoothstep(0.0, 0.08, h) * smoothstep(1.0, 0.45, h);
  float d = clamp(remap(shape, 1.0 - coverage, 1.0, 0.0, 1.0), 0.0, 1.0) * coverage;
  if (d <= 0.0 || !detail) return d;
  vec4 e = textureLod(cloudNoise, q * 0.75, 0.0);
  float erosion = e.g * 0.625 + e.b * 0.25 + e.a * 0.125;
  // Bordas: em baixo fiapos, em cima ondulado (inversão do Worley com a altura).
  erosion = mix(erosion, 1.0 - erosion, clamp(h * 4.0, 0.0, 1.0));
  return clamp(remap(d, erosion * 0.3, 1.0, 0.0, 1.0), 0.0, 1.0);
}

float cloudHeightFraction(vec3 p) {
  return (length(p) - (R_GROUND + CLOUD_BOTTOM)) / (CLOUD_TOP - CLOUD_BOTTOM);
}

// Interseções de um raio (origem dentro da atmosfera) com uma esfera de raio r centrada no planeta: t das duas raízes.
bool raySphere(vec3 o, vec3 d, float r, out float t0, out float t1) {
  float b = dot(o, d), c = dot(o, o) - r * r, disc = b * b - c;
  if (disc < 0.0) return false;
  float s = sqrt(disc);
  t0 = -b - s;
  t1 = -b + s;
  return true;
}

// Trecho do raio dentro da camada (para um observador abaixo dela).
bool cloudSegment(vec3 o, vec3 d, out float tStart, out float tEnd) {
  float a0, a1, b0, b1;
  if (!raySphere(o, d, R_GROUND + CLOUD_TOP, b0, b1)) return false;
  bool inner = raySphere(o, d, R_GROUND + CLOUD_BOTTOM, a0, a1);
  float r = length(o);
  if (r < R_GROUND + CLOUD_BOTTOM) {  // abaixo da camada: entra pela base, sai pelo topo
    tStart = inner ? a1 : 0.0;
    tEnd = b1;
  } else if (r < R_GROUND + CLOUD_TOP) {  // dentro
    tStart = 0.0;
    tEnd = (inner && a0 > 0.0) ? a0 : b1;
  } else {
    tStart = max(b0, 0.0);
    tEnd = (inner && a0 > 0.0) ? a0 : b1;
  }
  // O chão bloqueia o que estiver abaixo do horizonte.
  float g0, g1;
  if (raySphere(o, d, R_GROUND, g0, g1) && g0 > 0.0) tEnd = min(tEnd, g0);
  return tEnd > tStart;
}

// Fase de Henyey-Greenstein com duas lobas (forte para a frente = borda prateada contra o sol; um pouco para trás).
float cloudPhase(float cosTheta, float k) {
  return mix(henyeyGreenstein(cosTheta, -0.25 * k), henyeyGreenstein(cosTheta, 0.8 * k), 0.75);
}

// Profundidade óptica até o sol a partir de p (6 passos crescentes, sem detalhe: só a forma grossa importa).
float cloudOpticalDepthToLight(vec3 p, vec3 L) {
  float tau = 0.0, step = 0.08, t = 0.5 * step;
  for (int i = 0; i < 6; ++i) {
    vec3 q = p + L * t;
    tau += cloudDensity(q, cloudHeightFraction(q), false) * CLOUD_EXTINCTION * step;
    t += step;
    step *= 1.6;
  }
  return tau;
}

// Marcha de raio: luz espalhada pelas nuvens (pré-exposta) e a transmitância delas ao longo de (o, d).
//   jitter em [0,1): desloca o 1º passo (o ruído vira grão fino que a média temporal tira).
//   ambient: radiância do céu que ilumina as nuvens por baixo e pelos lados.
void cloudMarch(vec3 o, vec3 d, float jitter, vec3 ambient, out vec3 scattered, out float transmittance) {
  scattered = vec3(0.0);
  transmittance = 1.0;
  float tStart, tEnd;
  if (cloudCoverage() <= 0.0 || !cloudSegment(o, d, tStart, tEnd)) return;
  tEnd = min(tEnd, tStart + 30.0);  // até 30 km de nuvem atravessada
  if (tStart > 120.0) return;       // além disso a névoa da atmosfera esconde
  const int STEPS = 64;
  float dt = (tEnd - tStart) / float(STEPS);
  vec3 L = frame.sunDir.xyz;
  float cosTheta = dot(d, L);
  for (int i = 0; i < STEPS && transmittance > 0.01; ++i) {
    float t = tStart + (float(i) + jitter) * dt;
    vec3 p = o + d * t;
    float dens = cloudDensity(p, cloudHeightFraction(p), true);
    if (dens <= 0.0) continue;
    float sigmaT = dens * CLOUD_EXTINCTION, sigmaS = sigmaT * CLOUD_ALBEDO;
    // Luz do sol (ou da lua) que chega ao ponto, atenuada pela atmosfera e pela própria nuvem.
    vec3 sunE = frame.sunDir.w > 0.0 ? frame.sunDir.w * preExposure() * transmittanceToSun(p, L) : vec3(0.0);
    float tau = cloudOpticalDepthToLight(p, L);
    // Múltiplo espalhamento (Wrenninge 2013): oitavas com menos extinção, menos energia e fase mais isotrópica. Numa
    // nuvem (albedo 0,99, centenas de espalhamentos) a luz difundida domina: com só o 1º espalhamento o lado iluminado
    // de um cúmulo sairia ~10x mais escuro que o real (~20 mil nits ao sol, um difusor quase branco).
    float ms = 0.0, a = 1.0, b = 1.0, c = 1.0;
    for (int k = 0; k < 6; ++k) {
      ms += a * exp(-tau * b) * cloudPhase(cosTheta, c);
      a *= CLOUD_MS_A;
      b *= CLOUD_MS_B;
      c *= CLOUD_MS_C;
    }
    // Céu: chega de cima, atenuado pela nuvem acima do ponto (no miolo de uma nuvem densa quase não entra).
    float tauUp = cloudOpticalDepthToLight(p, normalize(p));
    vec3 S = sigmaS * (sunE * ms + ambient * exp(-tauUp * CLOUD_AMB_ATT) * CLOUD_AMB);
    // Integração analítica no passo (Hillaire 2016): conserva energia em passos grandes.
    float Tstep = exp(-sigmaT * dt);
    scattered += transmittance * (S - S * Tstep) / max(sigmaT, 1e-6);
    transmittance *= Tstep;
  }
  // Névoa da atmosfera entre o observador e a nuvem: nuvens distantes somem no horizonte.
  float haze = exp(-tStart / 60.0);
  scattered *= haze;
  transmittance = mix(1.0, transmittance, haze);
}

#endif
