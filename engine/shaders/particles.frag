#version 460
// Partícula: disco suave, esmaecida perto de superfícies ("soft particles": sem corte duro onde a fumaça encosta no
// chão) e escondida atrás delas. Saída pré-multiplicada (cor = src + dst × (1 − alfa)).
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(set = 1, binding = 1) uniform sampler2D sceneDepth;
layout(location = 0) in vec2 vCorner;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vViewDepth;
layout(location = 0) out vec4 outColor;

void main() {
  float r2 = dot(vCorner, vCorner);
  if (r2 >= 1.0) discard;
  float shape = (1.0 - r2) * (1.0 - r2);
  ivec2 px = ivec2(gl_FragCoord.xy);
  float d = texelFetch(sceneDepth, px, 0).r;
  float sceneDist = d > 0.0 ? linearDepth(d) : 1e9;
  float soft = clamp((sceneDist - vViewDepth) / 0.4, 0.0, 1.0);
  if (soft <= 0.0) discard;
  float k = shape * soft;
  // Limite por partícula: à noite a pré-exposição é alta e uma chama (dezenas de milhares de nits) passaria do máximo
  // do float16 — e centenas delas somadas na mistura viram infinito, que o TAA espalharia pela imagem. 200 já é muito
  // acima do branco da tela (exposição ~1): o que passa disso aparece estourado de qualquer forma.
  outColor = vec4(min(vColor.rgb * k, vec3(200.0)), vColor.a * k);
}
