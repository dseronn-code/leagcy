#version 460
layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUv;
layout(set = 1, binding = 0) uniform sampler2D atlas;  // fontes: cobertura no canal R
layout(push_constant) uniform Push { vec2 size; uint srgbTarget; } pc;
layout(location = 0) out vec4 outColor;
vec3 srgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
void main() {
  float a = vColor.a * (vUv.x < 0.0 ? 1.0 : texture(atlas, vUv).r);
  // Cores do jogo em sRGB; se a saída é sRGB o hardware codifica de novo, então entra linear.
  vec3 c = pc.srgbTarget != 0u ? srgbToLinear(vColor.rgb) : vColor.rgb;
  outColor = vec4(c * a, a);  // pré-multiplicada
}
