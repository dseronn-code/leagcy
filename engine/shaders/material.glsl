// Avaliação de material. Quem inclui define MATERIAL_SAMPLE(índice, uv):
//   fragment: texture() (mip automático) | compute/ray tracing: textureLod().
// Saída comum a todos os modelos: cor difusa + f0/f90 do especular + rugosidade (o que o BRDF usa).
#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

#include "common.glsl"
#include "scene.glsl"

struct Surface {
  vec3 diffuseColor;
  vec3 f0;
  float f90;
  float alpha;
  float roughness;       // perceptual
  vec3 emissive;
  vec3 normal;
  float normalVariance;  // variância do normal map no mip lido (Toksvig) — alarga a rugosidade
};

// N: normal geométrica interpolada; T: tangente (w = sinal da bitangente); T = 0 desliga o normal map.
Surface evalMaterial(Material m, vec3 P, vec3 N, vec4 T, vec2 uv) {
  Surface s;
  vec4 base = m.baseColor;
  if (m.baseColorTex >= 0) base *= MATERIAL_SAMPLE(m.baseColorTex, uv);
  s.alpha = base.a;
  s.emissive = m.emissive;
  if (m.emissiveTex >= 0) s.emissive *= MATERIAL_SAMPLE(m.emissiveTex, uv).rgb;

  if ((m.flags & MATERIAL_SPECULAR_GLOSSINESS) != 0u) {
    // Fluxo especular/glossiness: cor difusa e especular são dadas diretamente.
    vec4 sg = m.metalRoughTex >= 0 ? MATERIAL_SAMPLE(m.metalRoughTex, uv) : vec4(1.0);
    s.diffuseColor = base.rgb;
    s.f0 = m.specularColor * sg.rgb;
    s.f90 = 1.0;
    s.roughness = 1.0 - (1.0 - m.roughness) * sg.a;
  } else {
    // Metálico/rugosidade (glTF) + KHR_materials_specular + KHR_materials_ior.
    float metallic = m.metallic, roughness = m.roughness;
    if (m.metalRoughTex >= 0) {
      vec4 mr = MATERIAL_SAMPLE(m.metalRoughTex, uv);
      roughness *= mr.g;
      metallic *= mr.b;
    }
    float specWeight = m.specular * (m.specularTex >= 0 ? MATERIAL_SAMPLE(m.specularTex, uv).a : 1.0);
    vec3 specColor = m.specularColor * (m.specularColorTex >= 0 ? MATERIAL_SAMPLE(m.specularColorTex, uv).rgb : vec3(1.0));
    float r = (m.ior - 1.0) / (m.ior + 1.0);
    vec3 dielectricF0 = min(r * r * specColor, vec3(1.0)) * specWeight;
    s.diffuseColor = base.rgb * (1.0 - metallic);
    s.f0 = mix(dielectricF0, base.rgb, metallic);
    s.f90 = mix(specWeight, 1.0, metallic);
    s.roughness = roughness;
  }

  s.normal = N;
  s.normalVariance = 0.0;
  vec3 t = T.xyz - N * dot(N, T.xyz);
  if (m.normalTex >= 0 && dot(t, t) > 1e-8) {
    t = normalize(t);
    vec3 b = cross(N, t) * (T.w < 0.0 ? -1.0 : 1.0);
    vec3 n = MATERIAL_SAMPLE(m.normalTex, uv).xyz * 2.0 - 1.0;
    // Normal map em BC5 (padrão; texture_compress.h): só x e y — o canal azul vem 0 (n.z = -1, impossível num normal
    // map de espaço tangente) e o z é reconstruído da normal unitária. Em RGBA8 (--no-texture-compression) o z vem do
    // arquivo e o Toksvig abaixo também mede a variância guardada no encurtamento dos mips.
    if (n.z < -0.99) n.z = sqrt(max(1.0 - dot(n.xy, n.xy), 0.0));
    // Toksvig: nos mips a média de normais divergentes encurta o vetor; o encurtamento mede a
    // variância das normais dentro do pixel e vira rugosidade extra (sem cintilação a distância).
    float len = length(n);
    s.normalVariance = len > 1e-4 ? clamp((1.0 - len) / len, 0.0, 1.0) : 0.0;
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
    s.diffuseColor = mix(mix(grass, dirt, dirtMask), rock, rockMask);
    s.f0 = vec3(0.04);
    s.f90 = 1.0;
    s.roughness = mix(mix(0.9, 0.95, dirtMask), 0.7, rockMask);
  }
  s.roughness = clamp(s.roughness, 0.0, 1.0);
  return s;
}

#endif
