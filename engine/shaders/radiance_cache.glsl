// Cache de radiância no espaço do mundo (inspirado no SHaRC da NVIDIA — "Spatially Hashed Radiance Cache", usado no
// path tracing do Cyberpunk 2077). Tabela hash de células: posição quantizada (o tamanho da célula cresce com a
// distância à câmera) + direção dominante da normal (os dois lados de uma parede fina não se misturam).
//
// Cada célula guarda M = média da radiância incidente ponderada pelo cosseno, SEM o sol e as luzes locais (que entram
// por amostragem explícita, "next event estimation") — exatamente a parte que o motor aproximava com "céu sem
// oclusão". A luz difusa indireta que sai de um ponto é então albedo × peso difuso × M. Como as amostras usadas para
// atualizar uma célula também consultam o cache no ponto seguinte, a luz se propaga rebote a rebote entre frames:
// iluminação indireta com infinitos rebotes nos pontos atingidos por raios (reflexos e GI).
//
// Atualização: cada amostra soma em ponto fixo (atomicAdd) na célula; o passe radiance_cache_resolve.comp tira a média
// do frame e a mistura no histórico (32 frames), guardada em nits (independente da pré-exposição).
#ifndef RADIANCE_CACHE_GLSL
#define RADIANCE_CACHE_GLSL

#include "frame.glsl"

const uint RC_SIZE = 1u << 20;       // células (48 MB)
const uint RC_PROBES = 8u;           // sondagem linear
const float RC_CELL = 0.15;          // m: tamanho da célula perto da câmera (dobra a cada faixa de distância)
const float RC_FIXED = 1024.0;       // escala do ponto fixo das somas (radiância pré-exposta)

struct CacheEntry {
  uint key;        // impressão digital da célula (0 = vazia)
  uint lastFrame;  // último frame com amostras (células abandonadas são liberadas)
  uint pad0, pad1;
  uvec4 accum;     // soma do frame: rgb em ponto fixo (pré-exposta), a = nº de amostras
  vec4 resolved;   // rgb = M em nits, a = frames de histórico (0 = sem dados)
};
layout(set = 0, binding = 14, std430) buffer RadianceCache { CacheEntry rcEntries[]; };

// Célula de um ponto: nível de detalhe pela distância à câmera (0–4 m: 15 cm; 4–12 m: 30 cm; 12–28 m: 60 cm...).
void rcCell(vec3 P, vec3 N, out uint index, out uint key) {
  float level = floor(log2(1.0 + distance(P, frame.cameraPos.xyz) / 4.0));
  float size = RC_CELL * exp2(level);
  ivec3 q = ivec3(floor(P / size));
  vec3 a = abs(N);
  uint axis = a.x > a.y && a.x > a.z ? 0u : (a.y > a.z ? 1u : 2u);
  uint side = (axis == 0u ? N.x : axis == 1u ? N.y : N.z) < 0.0 ? 1u : 0u;
  uvec3 h = pcg3d(uvec3(q) ^ uvec3(0u, uint(level) * 0x9E3779B9u, (axis * 2u + side) * 0x85EBCA6Bu));
  index = h.x & (RC_SIZE - 1u);
  key = h.y | 1u;
}

// M (pré-exposto) na célula do ponto; falso se a célula ainda não tem dados.
bool rcQuery(vec3 P, vec3 N, out vec3 M) {
  M = vec3(0.0);
  if (!radianceCacheEnabled()) return false;
  uint index, key;
  rcCell(P, N, index, key);
  for (uint i = 0u; i < RC_PROBES; ++i) {
    uint slot = (index + i) & (RC_SIZE - 1u);
    uint k = rcEntries[slot].key;
    if (k == key) {
      vec4 r = rcEntries[slot].resolved;
      M = r.rgb * preExposure();
      return r.a > 0.0;
    }
    if (k == 0u) break;
  }
  M = vec3(0.0);
  return false;
}

// Soma uma amostra de radiância incidente (pré-exposta, já limitada contra vagalumes) na célula do ponto.
void rcAdd(vec3 P, vec3 N, vec3 Li) {
  if (!radianceCacheEnabled() || any(isnan(Li)) || any(isinf(Li))) return;
  uint index, key;
  rcCell(P, N, index, key);
  for (uint i = 0u; i < RC_PROBES; ++i) {
    uint slot = (index + i) & (RC_SIZE - 1u);
    uint prev = atomicCompSwap(rcEntries[slot].key, 0u, key);
    if (prev == 0u || prev == key) {
      uvec3 v = uvec3(min(Li, vec3(256.0)) * RC_FIXED + 0.5);  // teto: sem estouro de 32 bits nas somas
      atomicAdd(rcEntries[slot].accum.r, v.r);
      atomicAdd(rcEntries[slot].accum.g, v.g);
      atomicAdd(rcEntries[slot].accum.b, v.b);
      atomicAdd(rcEntries[slot].accum.a, 1u);
      return;
    }
  }
}

#endif
