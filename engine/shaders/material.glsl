// Avaliação de material — usada tanto na rasterização quanto nos pontos atingidos por raios.
#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

#include "common.glsl"

const uint MATERIAL_STANDARD = 0u;
const uint MATERIAL_TERRAIN = 1u;

struct Surface {
  vec3 albedo;
  float roughness;
  float metallic;
  vec3 emissive;
};

Surface evalMaterial(vec4 albedoRough, vec4 emissiveMetal, uint type, vec3 P, vec3 N) {
  Surface s;
  s.albedo = albedoRough.rgb;
  s.roughness = albedoRough.a;
  s.metallic = emissiveMetal.a;
  s.emissive = emissiveMetal.rgb;
  if (type == MATERIAL_TERRAIN) {
    // Terreno procedural: grama, terra e rocha misturadas por inclinação e ruído.
    float slope = 1.0 - N.y;
    float large = fbm(P.xz * 0.06, 5);
    float detail = fbm(P.xz * 1.7, 4);
    vec3 grass = mix(vec3(0.07, 0.11, 0.03), vec3(0.17, 0.19, 0.06), large);
    grass *= 0.75 + 0.5 * detail;
    vec3 dirt = vec3(0.21, 0.15, 0.10) * (0.7 + 0.6 * detail);
    vec3 rock = vec3(0.28, 0.27, 0.25) * (0.6 + 0.8 * detail);
    float dirtMask = smoothstep(0.55, 0.72, large + 0.15 * detail);
    float rockMask = smoothstep(0.18, 0.38, slope + (detail - 0.5) * 0.25);
    s.albedo = mix(mix(grass, dirt, dirtMask), rock, rockMask);
    s.roughness = mix(mix(0.9, 0.95, dirtMask), 0.7, rockMask);
    s.metallic = 0.0;
  }
  return s;
}

#endif
