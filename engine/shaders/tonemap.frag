#version 460
layout(set = 0, binding = 1, rgba32f) uniform readonly image2D accum;
layout(push_constant) uniform Push { uint accumFrame; float exposure; uint manualSrgb; uint pad; } pc;
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// ACES filmic (ajuste de Stephen Hill) — curva usada em muitos jogos AAA.
const mat3 ACES_IN = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
const mat3 ACES_OUT = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
vec3 rrtOdt(vec3 v) {
  vec3 a = v * (v + 0.0245786) - 0.000090537;
  vec3 b = v * (0.983729 * v + 0.4329510) + 0.238081;
  return a / b;
}
vec3 acesFitted(vec3 c) { return clamp(ACES_OUT * rrtOdt(ACES_IN * c), 0.0, 1.0); }

vec3 linearToSrgb(vec3 c) {
  return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}

void main() {
  vec3 hdr = imageLoad(accum, ivec2(gl_FragCoord.xy)).rgb * pc.exposure;
  vec3 c = acesFitted(hdr);
  vec2 d = vUV - 0.5;
  c *= 1.0 - 0.35 * dot(d, d);  // vinheta sutil
  if (pc.manualSrgb != 0u) c = linearToSrgb(c);
  outColor = vec4(c, 1.0);
}
