#version 460
// Final: mistura do bloom, exposição (automática), ACES filmic e vinheta.
layout(set = 1, binding = 0) uniform sampler2D colorIn;
layout(set = 1, binding = 1) uniform sampler2D bloomIn;
// x = exposição deste frame, w = pré-exposição com que a imagem foi gravada.
layout(set = 1, binding = 2, std430) readonly buffer ExposureState { vec4 state; };
layout(push_constant) uniform Push { float bloomStrength; float bloomNorm; uint manualSrgb; uint debugView; } pc;
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

void main() {
  vec3 hdr = textureLod(colorIn, vUV, 0.0).rgb;
  if (pc.debugView != 0u) {  // depuração: valor cru, sem exposição nem curva
    vec3 c = clamp(hdr, 0.0, 1.0);
    outColor = vec4(pc.manualSrgb != 0u ? linearToSrgb(c) : c, 1.0);
    return;
  }
  vec3 bloom = textureLod(bloomIn, vUV, 0.0).rgb * pc.bloomNorm;
  hdr = mix(hdr, bloom, pc.bloomStrength);  // mistura que conserva energia
  vec3 c = acesFitted(hdr * (state.x / max(state.w, 1e-20)));
  vec2 d = vUV - 0.5;
  c *= 1.0 - 0.35 * dot(d, d);
  if (pc.manualSrgb != 0u) c = linearToSrgb(c);
  outColor = vec4(c, 1.0);
}
