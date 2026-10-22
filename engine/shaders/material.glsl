// Avaliação de material. Quem inclui define MATERIAL_SAMPLE(índice, uv):
//   fragment: texture() (mip automático) | compute/ray tracing: textureLod().
#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

#include "common.glsl"
#include "scene.glsl"

struct Surface {
  vec3 albedo;
  float alpha;
  float roughness;
  float metallic;
  vec3 emissive;
  vec3 normal;
};

// N: normal geométrica interpolada; T: tangente (w = sinal da bitangente).
Surface evalMaterial(Material m, vec3 P, vec3 N, vec4 T, vec2 uv) {
  Surface s;
  vec4 base = m.baseColor;
  if (m.baseColorTex >= 0) base *= MATERIAL_SAMPLE(m.baseColorTex, uv);
  s.albedo = base.rgb;
  s.alpha = base.a;
  s.roughness = m.roughness;
  s.metallic = m.metallic;
  if (m.metalRoughTex >= 0) {
    vec4 mr = MATERIAL_SAMPLE(m.metalRoughTex, uv);
    s.roughness *= mr.g;
    s.metallic *= mr.b;
  }
  s.emissive = m.emissive;
  if (m.emissiveTex >= 0) s.emissive *= MATERIAL_SAMPLE(m.emissiveTex, uv).rgb;
  s.normal = N;
  // T = vec4(0) desliga o normal mapping (ex.: pontos atingidos por raios, sem tangente).
  vec3 t = T.xyz - N * dot(N, T.xyz);
  if (m.normalTex >= 0 && dot(t, t) > 1e-8) {
    t = normalize(t);
    vec3 b = cross(N, t) * (T.w < 0.0 ? -1.0 : 1.0);
    vec3 n = MATERIAL_SAMPLE(m.normalTex, uv).xyz * 2.0 - 1.0;
    n.xy *= m.normalScale;
    vec3 mapped = t * n.x + b * n.y + N * max(n.z, 1e-3);
    if (dot(mapped, mapped) > 1e-8) s.normal = normalize(mapped);
  }

  if ((m.flags & MATERIAL_TERRAIN) != 0u) {
    // Terreno procedural: grama, terra e rocha por inclinação e ruído.
    float slope = 1.0 - N.y;
    float large = fbm(P.xz * 0.06, 5);
    float detail = fbm(P.xz * 1.7, 4);
    vec3 grass = mix(vec3(0.07, 0.11, 0.03), vec3(0.17, 0.19, 0.06), large) * (0.75 + 0.5 * detail);
    vec3 dirt = vec3(0.21, 0.15, 0.10) * (0.7 + 0.6 * detail);
    vec3 rock = vec3(0.28, 0.27, 0.25) * (0.6 + 0.8 * detail);
    float dirtMask = smoothstep(0.55, 0.72, large + 0.15 * detail);
    float rockMask = smoothstep(0.18, 0.38, slope + (detail - 0.5) * 0.25);
    s.albedo = mix(mix(grass, dirt, dirtMask), rock, rockMask);
    s.roughness = mix(mix(0.9, 0.95, dirtMask), 0.7, rockMask);
    s.metallic = 0.0;
  }
  s.roughness = clamp(s.roughness, 0.03, 1.0);
  return s;
}

#endif
