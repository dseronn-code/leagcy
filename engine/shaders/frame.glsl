// Dados por frame (set 0) — espelha FrameUniforms em renderer.cpp.
#ifndef FRAME_GLSL
#define FRAME_GLSL

#include "common.glsl"
#include "atmosphere.glsl"

layout(set = 0, binding = 0) uniform FrameData {
  mat4 view;
  mat4 proj;
  mat4 viewProj;
  mat4 invViewProj;
  vec4 cameraPos;   // xyz = posição (m), w = tempo (s)
  vec4 sunDir;      // xyz = direção PARA o sol, w = irradiância solar
  vec4 params;      // x = raio angular do sol (rad), y = exposição, z = densidade da névoa, w = queda da névoa
  uvec4 frameInfo;  // x = semente do frame, y = ray tracing ativo
} frame;

layout(set = 0, binding = 1) uniform sampler2D skyViewLut;

// Posição em km relativa ao centro do planeta (câmera sobre o "polo norte").
vec3 planetPos(vec3 worldMeters) {
  return vec3(0.0, R_GROUND + max(worldMeters.y * 0.001, 0.0) + 0.002, 0.0);
}

// Parametrização da sky-view LUT (Hillaire 2020): azimute linear, latitude não linear
// para concentrar resolução no horizonte.
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

vec3 sampleSky(vec3 dir) { return textureLod(skyViewLut, skyLutUv(dir), 0.0).rgb; }

vec3 sunIrradianceAt(vec3 worldPos) {
  return frame.sunDir.w * transmittanceToSun(planetPos(worldPos), frame.sunDir.xyz);
}

// Névoa exponencial por altura com in-scattering do sol e do céu (a "neblina dourada").
vec3 applyHeightFog(vec3 color, vec3 worldPos) {
  float density = frame.params.z, falloff = frame.params.w;
  vec3 ro = frame.cameraPos.xyz;
  vec3 d = worldPos - ro;
  float dist = length(d);
  vec3 v = d / max(dist, 1e-4);
  // Integral analítica de density * exp(-falloff * y) ao longo do raio.
  float k = falloff * d.y;
  float lineIntegral = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
  float opticalDepth = density * exp(-falloff * ro.y) * lineIntegral * dist;
  float T = exp(-opticalDepth);

  float mu = dot(v, frame.sunDir.xyz);
  vec3 sunLight = sunIrradianceAt(ro) * henyeyGreenstein(mu, 0.65);
  vec3 ambient = sampleSky(normalize(vec3(v.x, 0.15, v.z)));
  return color * T + (sunLight + ambient) * (1.0 - T);
}

#endif
