// Dados globais do frame (set 0, bindings 0-1) — espelha FrameUniforms em renderer.cpp.
#ifndef FRAME_GLSL
#define FRAME_GLSL

#include "common.glsl"
#include "atmosphere.glsl"

layout(set = 0, binding = 0) uniform FrameData {
  mat4 view;
  mat4 proj;                  // com jitter de subpixel (TAA)
  mat4 viewProj;              // com jitter
  mat4 invViewProj;           // com jitter
  mat4 viewProjNoJitter;
  mat4 prevViewProjNoJitter;
  vec4 cameraPos;             // xyz = posição (m), w = near plane
  vec4 prevCameraPos;
  vec4 sunDir;                // xyz = direção PARA o sol, w = iluminância solar no topo da atmosfera (lux)
  vec4 params;                // x = raio angular do sol, y = dt (s), z = densidade da névoa, w = queda da névoa
  vec4 jitter;                // xy = deslocamento do jitter em UV, zw = do frame anterior
  vec4 renderSize;            // xy = resolução interna, zw = 1/xy
  vec4 outputSize;            // xy = resolução de saída, zw = 1/xy
  uvec4 frameInfo;            // x = índice do frame, y = ray tracing, z = nº de luzes, w = histórico válido
  uvec4 quality0;             // x = amostras por pixel, y = candidatos ReSTIR, z = vizinhos ReSTIR, w = amostras do DOF
  vec4 quality1;              // x = mip das texturas nos raios, y = limite de vagalumes, z = amostras do motion blur,
                              // w = compensação de energia do BRDF (1 = ligada)
  vec4 environment;           // x = céu uniforme em nits (> 0: teste da fornalha branca)
} frame;

bool energyCompensationEnabled() { return frame.quality1.w > 0.5; }

layout(set = 0, binding = 1) uniform sampler2D skyViewLut;

// Exposição (Frostbite/UE): toda radiância é guardada já multiplicada pela exposição do frame
// anterior ("pré-exposição"), mantendo valores de 120.000 lux a 0,001 nit dentro do float16.
// x = exposição mais recente (= pré-exposição deste frame), w = pré-exposição do frame anterior.
layout(set = 0, binding = 8, std430) readonly buffer GlobalExposure { vec4 exposureState; };
float preExposure() { return exposureState.x; }
// Fator para trazer históricos (denoiser, TAA) gravados no frame anterior para a escala atual.
float historyExposureScale() { return exposureState.w > 0.0 ? exposureState.x / exposureState.w : 1.0; }
// Maior valor seguro num alvo float16 (65504), com folga para filtros que somam vizinhos.
const float HALF_SAFE_MAX = 60000.0;
vec3 clampHalf(vec3 c) { return min(c, vec3(HALF_SAFE_MAX)); }

bool historyValid() { return frame.frameInfo.w != 0u; }

// Reverse-Z infinito: profundidade = near / distância.
float linearDepth(float d) { return frame.cameraPos.w / max(d, 1e-7); }

vec3 worldPosFromDepth(vec2 uv, float depth) {
  vec4 p = frame.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
  return p.xyz / p.w;
}
vec3 viewDirFromUv(vec2 uv) {
  return normalize(worldPosFromDepth(uv, 0.5) - frame.cameraPos.xyz);
}

// Posição em km relativa ao centro do planeta.
vec3 planetPos(vec3 worldMeters) {
  return vec3(0.0, R_GROUND + max(worldMeters.y * 0.001, 0.0) + 0.002, 0.0);
}

// Sky-view LUT (Hillaire 2020): azimute linear, latitude não linear (resolução no horizonte).
vec2 skyLutUv(vec3 dir) {
  float az = atan(dir.z, dir.x);
  float lat = asin(clamp(dir.y, -1.0, 1.0));
  float v = 0.5 + 0.5 * sign(lat) * sqrt(abs(lat) / (0.5 * PI));
  return vec2(fract(az / TWO_PI), v);
}
vec3 lutDirection(vec2 uv) {
  float az = uv.x * TWO_PI;
  float vv = uv.y * 2.0 - 1.0;
  float lat = sign(vv) * vv * vv * 0.5 * PI;
  return vec3(cos(lat) * cos(az), sin(lat), cos(lat) * sin(az));
}

// Radiância do céu (nits), já pré-exposta: a LUT é escrita com a pré-exposição do frame.
vec3 sampleSky(vec3 dir) { return textureLod(skyViewLut, skyLutUv(dir), 0.0).rgb; }

// Iluminância do sol (lux) chegando ao ponto, já pré-exposta.
vec3 sunIrradianceAt(vec3 worldPos) {
  if (frame.sunDir.w <= 0.0) return vec3(0.0);
  return frame.sunDir.w * preExposure() * transmittanceToSun(planetPos(worldPos), frame.sunDir.xyz);
}

vec3 sunDisk(vec3 dir) {
  float cosR = cos(frame.params.x);
  float mu = dot(dir, frame.sunDir.xyz);
  if (mu <= cosR || frame.sunDir.w <= 0.0) return vec3(0.0);
  float r = sqrt(max(0.0, 1.0 - mu * mu)) / sin(frame.params.x);
  float limb = 1.0 - 0.6 * (1.0 - sqrt(max(0.0, 1.0 - r * r)));  // escurecimento de borda
  return sunIrradianceAt(frame.cameraPos.xyz) / (TWO_PI * (1.0 - cosR)) * limb;
}

// Névoa exponencial por altura com in-scattering do sol e do céu.
vec3 applyHeightFog(vec3 color, vec3 worldPos) {
  float density = frame.params.z, falloff = frame.params.w;
  vec3 ro = frame.cameraPos.xyz;
  vec3 d = worldPos - ro;
  float dist = length(d);
  vec3 v = d / max(dist, 1e-4);
  float k = falloff * d.y;
  float lineIntegral = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
  float T = exp(-density * exp(-falloff * ro.y) * lineIntegral * dist);
  vec3 sunLight = sunIrradianceAt(ro) * henyeyGreenstein(dot(v, frame.sunDir.xyz), 0.65);
  vec3 ambient = sampleSky(normalize(vec3(v.x, 0.15, v.z)));
  return color * T + (sunLight + ambient) * (1.0 - T);
}

#endif
