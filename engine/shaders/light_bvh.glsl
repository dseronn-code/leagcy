// Amostragem de luzes por importância com a BVH de luzes (set 0, binding 13): ver renderer/light_bvh.h.
#ifndef LIGHT_BVH_GLSL
#define LIGHT_BVH_GLSL

#include "frame.glsl"

struct LightBvhNode {  // == renderer::LightBvhNode
  vec3 boundsMin;
  float power;
  vec3 boundsMax;
  float maxRange;
  uint rightOrLight;
  uint leaf;
  uint pad0, pad1;
};
layout(set = 0, binding = 13, std430) readonly buffer LightBvh { LightBvhNode lightBvh[]; };

// Importância de um nó visto do ponto P com normal N (Conty Estevez & Kulla 2018, sem cone de emissão:
// luzes esféricas emitem para todo lado). Conservadora: só dá zero se NENHUMA luz do nó pode iluminar P
// (fora do alcance de todas, ou a esfera envolvente inteira abaixo do horizonte de N).
float lightNodeImportance(LightBvhNode n, vec3 P, vec3 N) {
  vec3 dq = P - clamp(P, n.boundsMin, n.boundsMax);
  if (dot(dq, dq) >= n.maxRange * n.maxRange) return 0.0;
  vec3 c = 0.5 * (n.boundsMin + n.boundsMax);
  float rad = 0.5 * length(n.boundsMax - n.boundsMin);
  vec3 toC = c - P;
  float d2 = dot(toC, toC);
  float cosBound = 1.0;
  if (d2 > rad * rad) {
    float d = sqrt(d2);
    float sinB = rad / d, cosB = sqrt(max(1.0 - sinB * sinB, 0.0));
    float cosT = dot(N, toC / d), sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
    // cos(max(θ − θ_limite, 0)): o ponto da esfera envolvente mais próximo da normal.
    cosBound = cosT >= cosB ? 1.0 : cosT * cosB + sinT * sinB;
    if (cosBound <= 0.0) return 0.0;
  }
  return n.power * cosBound / max(d2, rad * rad);
}

// Desce a árvore escolhendo cada filho com probabilidade proporcional à importância.
// Devolve a luz e a probabilidade exata de tê-la escolhido.
bool sampleLightBvh(vec3 P, vec3 N, inout uint rng, out uint light, out float pmf) {
  light = 0u;
  pmf = 0.0;
  if (frame.frameInfo.z == 0u || lightNodeImportance(lightBvh[0], P, N) <= 0.0) return false;
  uint idx = 0u;
  float p = 1.0;
  for (int depth = 0; depth < 64; ++depth) {
    LightBvhNode n = lightBvh[idx];
    if (n.leaf != 0u) {
      light = n.rightOrLight;
      pmf = p;
      return p > 0.0;
    }
    uint left = idx + 1u, right = n.rightOrLight;
    float il = lightNodeImportance(lightBvh[left], P, N), ir = lightNodeImportance(lightBvh[right], P, N);
    if (il + ir <= 0.0) return false;
    float pl = il / (il + ir);
    if (rand(rng) < pl) {
      idx = left;
      p *= pl;
    } else {
      idx = right;
      p *= 1.0 - pl;
    }
  }
  return false;
}

#endif
