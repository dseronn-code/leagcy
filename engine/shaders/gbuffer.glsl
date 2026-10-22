// Leitura do G-buffer nos passes de compute (set 1, bindings 0-2 por convenção).
#ifndef GBUFFER_GLSL
#define GBUFFER_GLSL

#include "frame.glsl"

layout(set = 1, binding = 0) uniform sampler2D gDepth;
layout(set = 1, binding = 1) uniform sampler2D gNormal;  // xyz = normal, w = rugosidade
layout(set = 1, binding = 2) uniform sampler2D gAlbedo;  // rgb = albedo, a = metalicidade

struct GSample {
  float depth;
  vec3 P;
  vec3 N;
  vec3 V;
  float roughness;
  vec3 diffuseColor;
  vec3 f0;
};

GSample loadGBuffer(ivec2 id) {
  GSample g;
  g.depth = texelFetch(gDepth, id, 0).r;
  vec2 uv = (vec2(id) + 0.5) * frame.renderSize.zw;
  g.P = worldPosFromDepth(uv, g.depth);
  vec4 n = texelFetch(gNormal, id, 0);
  g.N = normalize(n.xyz);
  g.roughness = n.w;
  vec4 a = texelFetch(gAlbedo, id, 0);
  g.diffuseColor = a.rgb * (1.0 - a.a);
  g.f0 = mix(vec3(0.04), a.rgb, a.a);
  g.V = normalize(frame.cameraPos.xyz - g.P);
  return g;
}

bool insideRender(ivec2 p) { return all(greaterThanEqual(p, ivec2(0))) && all(lessThan(p, ivec2(frame.renderSize.xy))); }

#endif
