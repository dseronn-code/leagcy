// Avaliação de material. Quem inclui define MATERIAL_SAMPLE(índice, uv):
//   fragment: texture() (mip automático) | compute/ray tracing: textureLod().
// Saída comum a todos os modelos: cor difusa + f0/f90 do especular + rugosidade (o que o BRDF usa).
#ifndef MATERIAL_GLSL
#define MATERIAL_GLSL

#include "common.glsl"
#include "scene.glsl"
#include "frame.glsl"

struct Surface {
  vec3 diffuseColor;
  vec3 f0;
  float f90;
  float alpha;
  float roughness;       // perceptual
  vec3 emissive;
  vec3 normal;
  float normalVariance;  // variância do normal map no mip lido (Toksvig) — alarga a rugosidade
  float occlusion;       // oclusão do material (glTF occlusionTexture), 1 = nenhuma
  float transmission;    // KHR_materials_transmission: fração não refletida que atravessa (parede fina)
  vec3 transmissionTint; // cor base: tinge o que atravessa
  float clearcoat;       // KHR_materials_clearcoat: intensidade do verniz
  float clearcoatRoughness;
};

// Chuva (Lagarde 2012, "Water drop" 2–4, usado em Remember Me): a água entra nos poros e escurece o difuso
// (materiais porosos e ásperos escurecem mais), forma uma película que deixa a superfície lisa, e nas superfícies
// planas viradas para cima junta em poças — água por cima (f0 2%, quase espelho, normal plana) com os anéis dos
// pingos. As poças ficam onde um ruído de baixa frequência é alto (bordas suaves).
vec3 rainRipples(vec2 p, float t) {
  vec2 cell = floor(p), f = fract(p);
  vec3 n = vec3(0.0);
  for (int j = -1; j <= 1; ++j)
    for (int i = -1; i <= 1; ++i) {
      vec2 c = cell + vec2(i, j);
      uvec3 h = pcg3d(uvec3(ivec2(c) + ivec2(4096), 0x2c1b3c6du));
      vec3 r = vec3(h) * (1.0 / 4294967296.0);
      float phase = fract(t * (0.8 + 0.4 * r.z) + r.z);  // cada pingo num instante
      vec2 d = vec2(i, j) + r.xy - f;
      float dist = length(d);
      float radius = phase * 0.9;
      float ring = sin(clamp((dist - radius) * 18.0, -PI, PI)) * (1.0 - phase) * (1.0 - phase);
      ring *= smoothstep(0.12, 0.0, abs(dist - radius));
      n.xz += ring * (dist > 1e-4 ? d / dist : vec2(0.0));
    }
  return n * 0.6;
}

void applyWetness(inout Surface s, vec3 P, vec3 N) {
  float wet = frame.weather.x;
  float porosity = clamp((1.0 - dot(s.f0, vec3(0.333)) * 4.0) * (0.3 + 0.7 * s.roughness), 0.0, 1.0);
  s.diffuseColor *= mix(1.0, 0.35, wet * porosity);  // água nos poros: menos luz espalhada de volta
  s.roughness = mix(s.roughness, min(s.roughness, 0.25), wet * 0.8);
  // Poças: só em superfícies quase horizontais viradas para cima.
  float up = smoothstep(0.92, 0.98, N.y);
  if (up <= 0.0 || frame.weather.y <= 0.0) return;
  float n = fbm(P.xz * 0.35, 3) * 0.75 + fbm(P.xz * 2.1, 2) * 0.25;
  float puddle = up * smoothstep(1.0 - frame.weather.y * 0.6, 1.08 - frame.weather.y * 0.6, n + 0.15);
  if (puddle <= 0.0) return;
  s.diffuseColor *= mix(1.0, 0.25, puddle);  // a água por cima absorve e espelha
  s.f0 = mix(s.f0, vec3(0.02), puddle);
  s.f90 = mix(s.f90, 1.0, puddle);
  s.roughness = mix(s.roughness, 0.02, puddle);
  vec3 flatN = vec3(0.0, 1.0, 0.0);
  if (frame.weather.z > 0.0) flatN = normalize(flatN + rainRipples(P.xz * 3.0, frame.weather.w) * frame.weather.z);
  s.normal = normalize(mix(s.normal, flatN, puddle));
  s.normalVariance *= 1.0 - puddle;
}

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
  s.occlusion = m.occlusionTex >= 0 ? MATERIAL_SAMPLE(m.occlusionTex, uv).r : 1.0;
  s.transmission = m.transmission;
  if (m.transmissionTex >= 0) s.transmission *= MATERIAL_SAMPLE(m.transmissionTex, uv).r;
  s.transmissionTint = base.rgb;
  s.clearcoat = m.clearcoat;
  s.clearcoatRoughness = m.clearcoatRoughness;
  if (m.clearcoatTex >= 0) s.clearcoat *= MATERIAL_SAMPLE(m.clearcoatTex, uv).r;
  if (m.clearcoatRoughnessTex >= 0) s.clearcoatRoughness *= MATERIAL_SAMPLE(m.clearcoatRoughnessTex, uv).g;
  s.clearcoatRoughness = clamp(s.clearcoatRoughness, 0.0, 1.0);
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

  if (m.flakes > 0.0) {
    // Flocos metálicos: cada célula da grade de UV sorteia se tem floco e a inclinação dele (até ~20°). Visíveis um a
    // um quando a célula cobre ~1 pixel ou mais; menores que o pixel, a média deles é um lobo mais largo (variância
    // da normal, que o AA especular soma na rugosidade) — sem cintilar à distância.
    vec2 cellUv = uv * m.flakeScale;
    uvec3 h = pcg3d(uvec3(uvec2(ivec2(floor(cellUv)) + ivec2(1 << 20)), 0x9e3779b9u));
    vec3 r = vec3(h) * (1.0 / 4294967296.0);
    float tilt = 0.35 * sqrt(r.x), phi = TWO_PI * r.y;
    vec3 ft, fb;
    orthonormalBasis(s.normal, ft, fb);
    vec3 flakeN = normalize(s.normal + tilt * (cos(phi) * ft + sin(phi) * fb));
#ifdef MATERIAL_DERIVATIVES
    float footprint = max(length(dFdx(cellUv)), length(dFdy(cellUv)));
    float resolved = clamp(1.5 - footprint, 0.0, 1.0);
#else
    float resolved = 0.0;
#endif
    float isFlake = r.z < m.flakes ? 1.0 : 0.0;
    s.normal = normalize(mix(s.normal, flakeN, isFlake * resolved));
    s.normalVariance += m.flakes * 0.03 * (1.0 - resolved);  // ≈ variância da inclinação dos flocos
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
  if (frame.weather.x > 0.0 && s.transmission <= 0.0) applyWetness(s, P, N);
  s.diffuseColor *= 1.0 - s.transmission;  // o que atravessa não espalha (glTF: mix(difuso, transmissão))
  return s;
}

#endif
