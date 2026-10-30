#version 460
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Final: mistura do bloom, exposição (automática), ACES filmic, nitidez RCAS e vinheta.
layout(set = 1, binding = 0) uniform sampler2D colorIn;
layout(set = 1, binding = 1) uniform sampler2D bloomIn;
// x = exposição deste frame, w = pré-exposição com que a imagem foi gravada.
layout(set = 1, binding = 2, std430) readonly buffer ExposureState { vec4 state; };
layout(push_constant) uniform Push {
  float bloomStrength; float bloomNorm; uint manualSrgb; uint debugView;
  float sharpness;  // RCAS: 0 = desligado, 1 = máximo
  float lensFlare;  // 0 = sem reflexo nas lentes
} pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// ACES filmic (ajuste de Stephen Hill).
const mat3 ACES_IN = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
const mat3 ACES_OUT = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
vec3 rrtOdt(vec3 v) {
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}
vec3 acesFitted(vec3 c) { return clamp(ACES_OUT * rrtOdt(ACES_IN * c), 0.0, 1.0); }
vec3 linearToSrgb(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c)); }

// Reflexo nas lentes ("pseudo lens flare", Chapman 2013 — como em muitos jogos): as reflexões entre os elementos da
// objetiva projetam imagens "fantasmas" das fontes fortes espelhadas pelo centro da imagem, mais um halo em anel, com
// dispersão cromática. Feito sobre o bloom (já borrado) e modulado por uma "sujeira" da lente procedural. Sem limiar,
// como numa lente de verdade: toda a luz gera fantasmas com a mesma fração pequena (~0,8% cada, uma objetiva de vários elementos), e só as fontes muito
// mais fortes que a cena (sol, faróis, lâmpadas) ficam visíveis.
vec3 brightBloom(vec2 uv, float exposure) {
  return textureLod(bloomIn, clamp(uv, 0.0, 1.0), 0.0).rgb * pc.bloomNorm * exposure;
}
vec3 chromatic(vec2 uv, vec2 dir, float exposure) {
  const float D = 0.006;
  return vec3(brightBloom(uv - dir * D, exposure).r, brightBloom(uv, exposure).g, brightBloom(uv + dir * D, exposure).b);
}
vec3 lensFlare(vec2 uv, float exposure) {
  vec2 flipped = vec2(1.0) - uv;
  vec2 ghostVec = (vec2(0.5) - flipped) * 0.35;
  vec2 dir = normalize(ghostVec + 1e-6);
  vec3 sum = vec3(0.0);
  for (int i = 0; i < 6; ++i) {
    vec2 p = fract(flipped + ghostVec * float(i));
    float w = pow(1.0 - clamp(length(vec2(0.5) - p) / 0.7071, 0.0, 1.0), 6.0);  // fantasmas fortes perto do centro
    sum += chromatic(p, dir, exposure) * w * 0.008;
  }
  vec2 haloP = flipped + dir * 0.45;
  float hw = pow(1.0 - clamp(length(vec2(0.5) - fract(haloP)) / 0.7071, 0.0, 1.0), 5.0);
  sum += chromatic(haloP, dir, exposure) * hw * 0.01;
  // Sujeira da lente: manchas suaves que só aparecem contra a luz forte.
  float dirt = 0.35 + 0.65 * smoothstep(0.45, 0.75, fbm(uv * vec2(6.0, 3.5), 4));
  return sum * dirt * pc.lensFlare;
}

// HDR -> cor de tela (antes da vinheta) num ponto da imagem.
vec3 display(vec2 uv) {
  vec3 hdr = textureLod(colorIn, uv, 0.0).rgb;
  vec3 bloom = textureLod(bloomIn, uv, 0.0).rgb * pc.bloomNorm;
  hdr = mix(hdr, bloom, pc.bloomStrength);  // mistura que conserva energia
  float exposure = state.x / max(state.w, 1e-20);
  vec3 c = hdr * exposure;
  if (pc.lensFlare > 0.0) c += lensFlare(uv, exposure);
  return acesFitted(c);
}

// AMD FidelityFX RCAS ("Robust Contrast Adaptive Sharpening", FSR 1, licença MIT): nitidez em cruz que nunca
// passa do mínimo/máximo da vizinhança (sem halo) e é amortecida onde a vizinhança parece ruído.
vec3 rcas(vec2 uv, vec3 e) {
  vec2 t = 1.0 / vec2(textureSize(colorIn, 0));
  vec3 b = display(uv + vec2(0.0, -t.y)), d = display(uv + vec2(-t.x, 0.0));
  vec3 f = display(uv + vec2(t.x, 0.0)), h = display(uv + vec2(0.0, t.y));
  vec3 mn4 = min(min(b, d), min(f, h)), mx4 = max(max(b, d), max(f, h));
  const vec2 peakC = vec2(1.0, -4.0);
  vec3 hitMin = min(mn4, e) / (4.0 * mx4 + 1e-6);
  vec3 hitMax = (peakC.x - max(mx4, e)) / (4.0 * mn4 + peakC.y);
  vec3 lobeRGB = max(-hitMin, hitMax);
  const float RCAS_LIMIT = 0.25 - 1.0 / 16.0;
  float lobe = max(-RCAS_LIMIT, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * pc.sharpness;
  // Redução de ruído do RCAS: contraste "de pixel isolado" em relação à faixa da vizinhança.
  vec3 lw = vec3(0.5, 1.0, 0.5);
  float bL = dot(b, lw), dL = dot(d, lw), eL = dot(e, lw), fL = dot(f, lw), hL = dot(h, lw);
  float nz = 0.25 * (bL + dL + fL + hL) - eL;
  float range = max(max(max(bL, dL), max(fL, hL)), eL) - min(min(min(bL, dL), min(fL, hL)), eL);
  nz = clamp(abs(nz) / max(range, 1e-6), 0.0, 1.0);
  lobe *= -0.5 * nz + 1.0;
  return (lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0);
}

void main() {
  vec3 hdr = textureLod(colorIn, vUV, 0.0).rgb;
  if (pc.debugView != 0u) {  // depuração: valor cru, sem exposição nem curva
    vec3 c = clamp(hdr, 0.0, 1.0);
    outColor = vec4(pc.manualSrgb != 0u ? linearToSrgb(c) : c, 1.0);
    return;
  }
  vec3 c = display(vUV);
  if (pc.sharpness > 0.0) c = rcas(vUV, c);
  vec2 d = vUV - 0.5;
  c *= 1.0 - 0.35 * dot(d, d);
  if (pc.manualSrgb != 0u) c = linearToSrgb(c);
  outColor = vec4(c, 1.0);
}
