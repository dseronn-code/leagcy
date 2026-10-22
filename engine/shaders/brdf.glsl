// BRDF Cook-Torrance: GGX + Smith height-correlated + Fresnel-Schlick (padrão PBR de AAA).
#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "common.glsl"

float D_GGX(float NoH, float a) {
  float a2 = a * a;
  float d = (NoH * a2 - NoH) * NoH + 1.0;
  return a2 / (PI * d * d);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
  float a2 = a * a;
  float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
  float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-6);
}
vec3 F_Schlick(vec3 f0, float VoH) {
  float f = pow(1.0 - VoH, 5.0);
  return f0 + (1.0 - f0) * f;
}
float G1_Smith(float NoX, float a) {
  float a2 = a * a;
  return 2.0 * NoX / (NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX));
}

// BRDF * NoL para uma luz direcional.
vec3 evalBRDF(vec3 N, vec3 V, vec3 L, vec3 diffuseColor, vec3 f0, float roughness) {
  float NoL = max(dot(N, L), 0.0);
  if (NoL <= 0.0) return vec3(0.0);
  vec3 H = normalize(V + L);
  float NoV = max(dot(N, V), 1e-4), NoH = max(dot(N, H), 0.0), VoH = max(dot(V, H), 0.0);
  float a = max(roughness * roughness, 0.002);
  vec3 F = F_Schlick(f0, VoH);
  vec3 spec = D_GGX(NoH, a) * V_SmithGGXCorrelated(NoV, NoL, a) * F;
  vec3 diff = diffuseColor / PI * (1.0 - F);
  return (diff + spec) * NoL;
}

// Amostragem da distribuição de normais visíveis (VNDF) por calotas esféricas
// (Dupuy & Benyoub 2023) — Ve em espaço tangente.
vec3 sampleGGXVNDF(vec3 Ve, float a, vec2 u) {
  vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
  float phi = TWO_PI * u.x;
  float z = (1.0 - u.y) * (1.0 + Vh.z) - Vh.z;
  float sinT = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
  vec3 Nh = vec3(sinT * cos(phi), sinT * sin(phi), z) + Vh;
  return normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
}

#endif
