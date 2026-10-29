// Leitura do G-buffer nos passes de compute (set 1, bindings 0-3 por convenção).
// Quem inclui este arquivo começa os próprios bindings do set 1 em 4.
#ifndef GBUFFER_GLSL
#define GBUFFER_GLSL

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D gDepth;
layout(set = 1, binding = 1) uniform sampler2D gNormal;  // xyz = normal, w = rugosidade
layout(set = 1, binding = 2) uniform sampler2D gDiffuse;   // rgb = cor difusa
layout(set = 1, binding = 3) uniform sampler2D gSpecular;  // rgb = f0, a = f90

// Normal geométrica (faceta real do triângulo) reconstruída da profundidade, escolhendo o vizinho
// mais próximo em cada eixo para não atravessar silhuetas (Turánszki). É ela que decide para que lado
// os raios saem da superfície — a normal de shading (com normal map) pode atravessar a geometria.
vec3 geometricNormal(ivec2 id, vec3 P) {
  ivec2 maxP = ivec2(frame.renderSize.xy) - 1;
  vec3 pos[4];
  ivec2 offs[4] = ivec2[](ivec2(-1, 0), ivec2(1, 0), ivec2(0, -1), ivec2(0, 1));
  float dz[4];
  float zc = linearDepth(texelFetch(gDepth, id, 0).r);
  for (int i = 0; i < 4; ++i) {
    ivec2 q = clamp(id + offs[i], ivec2(0), maxP);
    float d = texelFetch(gDepth, q, 0).r;
    pos[i] = worldPosFromDepth((vec2(q) + 0.5) * frame.renderSize.zw, d);
    dz[i] = d > 0.0 ? abs(linearDepth(d) - zc) : 1e30;
  }
  vec3 dx = dz[0] < dz[1] ? P - pos[0] : pos[1] - P;
  vec3 dy = dz[2] < dz[3] ? P - pos[2] : pos[3] - P;
  vec3 n = cross(dx, dy);
  if (dot(n, n) < 1e-20) return vec3(0.0);
  n = normalize(n);
  return dot(n, frame.cameraPos.xyz - P) < 0.0 ? -n : n;
}

struct GSample {
  float depth;
  vec3 P;
  vec3 N;   // normal de shading (com normal map), virada para a câmera
  vec3 Ng;  // normal geométrica, virada para a câmera
  vec3 V;
  float roughness;
  vec3 diffuseColor;
  vec3 f0;
  float f90;
  float occlusion;  // oclusão do material: escurece só a luz indireta
};

GSample loadGBuffer(ivec2 id) {
  GSample g;
  g.depth = texelFetch(gDepth, id, 0).r;
  vec2 uv = (vec2(id) + 0.5) * frame.renderSize.zw;
  g.P = worldPosFromDepth(uv, g.depth);
  vec4 n = texelFetch(gNormal, id, 0);
  g.N = normalize(n.xyz);
  g.roughness = n.w;
  vec4 albedo = texelFetch(gDiffuse, id, 0);
  g.diffuseColor = albedo.rgb;
  g.occlusion = albedo.a;
  vec4 spec = texelFetch(gSpecular, id, 0);
  g.f0 = spec.rgb;
  g.f90 = spec.a;
  g.V = normalize(frame.cameraPos.xyz - g.P);
  g.Ng = geometricNormal(id, g.P);
  if (dot(g.Ng, g.Ng) == 0.0) g.Ng = dot(g.N, g.V) > 0.0 ? g.N : -g.N;
  // Normal de shading do lado visível: normal maps podem apontá-la para longe da câmera.
  float NoV = dot(g.N, g.V);
  if (NoV < 1e-3) g.N = normalize(g.N + g.V * (1e-3 - NoV));
  return g;
}

bool insideRender(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(frame.renderSize.xy))); }

#endif
