// Constantes, gerador de números aleatórios e ruído — compartilhado por todos os shaders.
#ifndef COMMON_GLSL
#define COMMON_GLSL

const float PI = 3.14159265359;
const float TWO_PI = 6.28318530718;

// PCG hash: gerador rápido e de boa qualidade estatística.
uint pcg(inout uint state) {
  state = state * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
float rand(inout uint state) { return float(pcg(state)) * (1.0 / 4294967296.0); }
vec2 rand2(inout uint state) { return vec2(rand(state), rand(state)); }

// Hash 3D robusto (Jarzynski & Olano, "Hash Functions for GPU Rendering", JCGT 2020).
uvec3 pcg3d(uvec3 v) {
  v = v * 1664525u + 1013904223u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  v ^= v >> 16u;
  v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
  return v;
}
// Semente de PCG independente por pixel, frame e uso (salt): sem padrões entre pixels vizinhos.
uint seedFor(uvec2 pixel, uint frameIndex, uint salt) { return pcg3d(uvec3(pixel, frameIndex ^ (salt * 0x9E3779B9u))).x | 1u; }

// ---- Sobol com embaralhamento de Owen por hash (Burley, "Practical Hash-based Owen Scrambling", JCGT 2020) ----
// Cada pixel recebe uma sequência de baixa discrepância própria ao longo dos frames: as amostras
// de frames consecutivos se complementam, e o denoiser/TAA convergem mais rápido que com ruído branco.
uint laineKarrasPermutation(uint x, uint seed) {
  x += seed;
  x ^= x * 0x6c50b47cu;
  x ^= x * 0xb82f1e52u;
  x ^= x * 0xc7afe638u;
  x ^= x * 0x8d22f6e6u;
  return x;
}
uint nestedUniformScramble(uint x, uint seed) {
  return bitfieldReverse(laineKarrasPermutation(bitfieldReverse(x), seed));
}
uint sobolDim1(uint index) {  // 2ª dimensão de Sobol (polinômio x + 1)
  uint result = 0u, v = 0x80000000u;
  for (; index != 0u; index >>= 1u, v ^= v >> 1u)
    if ((index & 1u) != 0u) result ^= v;
  return result;
}
struct SampleGen {
  uint pixelSeed;
  uint index;  // número da amostra na sequência do pixel (= frame)
};
SampleGen makeSampleGen(uvec2 pixel, uint frameIndex) { return SampleGen(pcg3d(uvec3(pixel, 0x5bd1e995u)).x, frameIndex); }
// Par 2D de Sobol para a dimensão `dim`; dimensões diferentes usam embaralhamentos independentes ("padding").
vec2 sample2D(SampleGen g, uint dim) {
  uint seed = pcg3d(uvec3(g.pixelSeed, dim, 0x68bc21ebu)).x;
  uint i = nestedUniformScramble(g.index, seed);
  uint x = nestedUniformScramble(bitfieldReverse(i), seed ^ 0xa511e9b3u);
  uint y = nestedUniformScramble(sobolDim1(i), seed ^ 0x63d83595u);
  return vec2(x >> 8u, y >> 8u) * (1.0 / 16777216.0);  // 24 bits: nunca chega a 1,0
}

float hash12(vec2 p) {
  vec3 p3 = fract(vec3(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
float valueNoise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 u = f * f * (3.0 - 2.0 * f);
  float a = hash12(i), b = hash12(i + vec2(1, 0)), c = hash12(i + vec2(0, 1)), d = hash12(i + vec2(1, 1));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p, int octaves) {
  float sum = 0.0, amp = 0.5;
  for (int i = 0; i < octaves; ++i) {
    sum += amp * valueNoise(p);
    p = p * 2.03 + vec2(17.1, 9.7);
    amp *= 0.5;
  }
  return sum;
}

// Base ortonormal a partir de uma normal (Duff et al. 2017).
void orthonormalBasis(vec3 n, out vec3 t, out vec3 b) {
  float s = n.z >= 0.0 ? 1.0 : -1.0;
  float a = -1.0 / (s + n.z);
  float k = n.x * n.y * a;
  t = vec3(1.0 + s * n.x * n.x * a, s * k, -s * n.x);
  b = vec3(k, s + n.y * n.y * a, -n.y);
}

vec3 cosineSampleHemisphere(vec3 n, vec2 u) {
  float r = sqrt(u.x), phi = TWO_PI * u.y;
  vec3 t, b;
  orthonormalBasis(n, t, b);
  return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u.x)));
}

// Direção uniforme dentro de um cone (usado para o disco solar => sombras suaves).
vec3 sampleCone(vec3 dir, float cosMax, vec2 u) {
  float cosT = mix(1.0, cosMax, u.x);
  float sinT = sqrt(max(0.0, 1.0 - cosT * cosT));
  float phi = TWO_PI * u.y;
  vec3 t, b;
  orthonormalBasis(dir, t, b);
  return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + dir * cosT);
}

#endif
