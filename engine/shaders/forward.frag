#version 460
// Renderer forward de celular (Settings::forward): cada objeto sai iluminado direto na imagem final, numa passada só —
// sol (GGX + difuso, sombra das cascatas), céu como luz ambiente (hemisfério), emissão, ACES e sRGB. Sem G-buffer,
// ReSTIR, denoiser nem TAA: é o que cabe em 16 ms numa GPU de celular. O vertex shader é o do G-buffer.
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"
// Streaming de texturas: o mesmo pedido de resolução do gbuffer.frag (um pixel em 16 anota quanto a tela pede).
void textureRequest(int i, vec2 uv) {
  const uint f = frame.frameInfo.x;
  if ((uint(gl_FragCoord.x) & 3u) == (f & 3u) && (uint(gl_FragCoord.y) & 3u) == ((f >> 2) & 3u)) {
    float size = float(max(textureFullSize(i) >> textureBase(i), 1u));
    float lod = textureQueryLod(textures[nonuniformEXT(i)], uv).y + textureMipBias();
    uint want = uint(clamp(size * exp2(-lod), 1.0, 65536.0));
    if (textureFeedback[i] < want) atomicMax(textureFeedback[i], want);
  }
}
vec4 sampleMaterialTexture(int i, vec2 uv) {
  textureRequest(i, uv);
  return texture(textures[nonuniformEXT(i)], uv, textureMipBias());
}
#define MATERIAL_SAMPLE(i, uv) sampleMaterialTexture(i, uv)
#define TERRAIN_SAMPLE(i, uv) texture(textures[i], uv, textureMipBias())
#define TERRAIN_REQUEST(i, uv) textureRequest(i, uv)
#define MATERIAL_DERIVATIVES
#include "material.glsl"

layout(push_constant) uniform Push { uvec2 drawList; uint manualSrgb; uint pad; } pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vTangent;
layout(location = 4) in vec4 vCurClip;
layout(location = 5) in vec4 vPrevClip;
layout(location = 6) flat in uint vObject;
layout(location = 7) flat in uint vMaterial;
layout(location = 8) flat in uint vLod;
layout(location = 9) in float vCoverage;
layout(location = 10) in vec4 vColor;
layout(location = 11) flat in uint vDither;

layout(location = 0) out vec4 outColor;

// ACES filmic (o mesmo do tonemap.frag).
const mat3 ACES_IN = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
const mat3 ACES_OUT = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
vec3 acesFitted(vec3 v) {
  v = ACES_IN * v;
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return clamp(ACES_OUT * (a / b), 0.0, 1.0);
}
vec3 linearToSrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

// Luz do céu num hemisfério em volta de N: média de 5 direções da LUT do céu (radiância pré-exposta) e o chão
// refletindo um pouco por baixo. Irradiância de um céu uniforme L é πL; o π cancela com o 1/π do difuso.
vec3 skyAmbient(vec3 N) {
  vec3 up = sampleSky(vec3(0.0, 1.0, 0.0));
  vec3 ring = 0.25 * (sampleSky(normalize(vec3(1.0, 0.5, 0.0))) + sampleSky(normalize(vec3(-1.0, 0.5, 0.0))) +
                      sampleSky(normalize(vec3(0.0, 0.5, 1.0))) + sampleSky(normalize(vec3(0.0, 0.5, -1.0))));
  vec3 sky = mix(ring, up, 0.4);
  vec3 ground = sky * 0.25;  // gramado/arquibancada devolvendo parte da luz
  return mix(ground, sky, clamp(N.y * 0.5 + 0.5, 0.0, 1.0));
}

float D_GGX(float NoH, float a) {
  float a2 = a * a;
  float d = NoH * NoH * (a2 - 1.0) + 1.0;
  return a2 / (PI * d * d);
}
float V_SmithFast(float NoV, float NoL, float a) {  // Hammon 2017 (aproximação barata do Smith correlacionado)
  return 0.5 / mix(2.0 * NoL * NoV, NoL + NoV, a);
}

void main() {
  Material m = materials[vMaterial];
  vec3 N = normalize(vNormal);
  if (!gl_FrontFacing) N = -N;
  Surface s = evalMaterial(m, vWorldPos, N, vTangent, vUV, vColor);
  const bool blend = (m.flags & MATERIAL_BLEND) != 0u;
  if (!blend && (m.flags & MATERIAL_ALPHA_MASK) != 0u && s.alpha < m.alphaCutoff) discard;
  if (blend && s.alpha <= 0.002) discard;

  vec3 V = normalize(frame.cameraPos.xyz - vWorldPos);
  vec3 Nn = s.normal;
  float NoV = max(dot(Nn, V), 1e-4);
  float a = max(s.roughness * s.roughness, 0.02);
  vec3 color = vec3(0.0);

  // Sol (lux pré-expostos) com a sombra das cascatas.
  vec3 L = frame.sunDir.xyz;
  float NoL = dot(Nn, L);
  if (NoL > 0.0 && frame.sunDir.w > 0.0) {
    vec3 E = sunIrradianceAt(vWorldPos);
    float vis = shadowMapsEnabled() ? sunShadowMap(vWorldPos, normalize(vNormal)) : 1.0;
    vec3 H = normalize(V + L);
    float NoH = max(dot(Nn, H), 0.0), VoH = max(dot(V, H), 0.0);
    vec3 F = s.f0 + (vec3(s.f90) - s.f0) * pow(1.0 - VoH, 5.0);
    vec3 spec = D_GGX(NoH, a) * V_SmithFast(NoV, NoL, a) * F;
    color += (s.diffuseColor * (1.0 / PI) + spec) * E * (NoL * vis);
  }
  // Céu: difuso pelo hemisfério, especular pela direção refletida (mais apagado quanto mais áspero).
  vec3 R = reflect(-V, Nn);
  vec3 Fa = s.f0 + (vec3(s.f90) - s.f0) * pow(1.0 - NoV, 5.0) * (1.0 - s.roughness);
  color += s.diffuseColor * skyAmbient(Nn) * s.occlusion;
  color += sampleSky(normalize(vec3(R.x, max(R.y, 0.05), R.z))) * Fa * (1.0 - s.roughness) * s.occlusion;
  color += s.emissive * preExposure();

  vec3 c = acesFitted(color);
  if (pc.manualSrgb != 0u) c = linearToSrgb(c);
  outColor = vec4(c, blend ? s.alpha : 1.0);
}
