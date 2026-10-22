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
