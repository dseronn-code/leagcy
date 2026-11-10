// Sondas de irradiância assadas (gi_bake.comp, com ray tracing; usadas sem ele): grade regular de sondas, cada uma
// com a irradiância em harmônicos esféricos L1 (já convolvida com o cosseno: E(n) = c0 + c1·n por canal) e a
// validade (0 = a sonda ficou dentro de uma parede: os raios dela viram o verso das faces).
// Cabeçalho: [0] = origem.xyz, espaçamento; [1] = dimensões x, y, z, 1 se há sondas. Por sonda: GI_STRIDE vec4 —
// R, G, B, validade e a visibilidade: distância média até as superfícies e a média do quadrado em 6 lados
// ([4] = média +x, −x, +y, −y; [5] = média +z, −z, quadrado +x, −x; [6] = quadrado +y, −y, +z, −z).
// Irradiância sem pré-exposição.
#ifndef GI_PROBES_GLSL
#define GI_PROBES_GLSL
layout(set = 0, binding = 26, std430) readonly buffer GiProbes { vec4 giHeader[2]; vec4 giData[]; };

const uint GI_STRIDE = 7u;
bool giProbesAvailable() { return giHeader[1].w > 0.5; }

// Visibilidade da sonda i até o ponto a `d` dela (Chebyshev sobre a distância média até as superfícies no lado de d,
// como o DDGI): perto de 1 se o ponto está antes das paredes que a sonda vê, cai rápido atrás delas.
float giProbeVisibility(uint i, vec3 d) {
  float dist = length(d);
  if (dist < 1e-4) return 1.0;
  vec3 u = d / dist, w = u * u;
  vec4 a = giData[i + 4u], b = giData[i + 5u], c = giData[i + 6u];
  float mean = w.x * (u.x >= 0.0 ? a.x : a.y) + w.y * (u.y >= 0.0 ? a.z : a.w) + w.z * (u.z >= 0.0 ? b.x : b.y);
  float mean2 = w.x * (u.x >= 0.0 ? b.z : b.w) + w.y * (u.y >= 0.0 ? c.x : c.y) + w.z * (u.z >= 0.0 ? c.z : c.w);
  if (dist <= mean) return 1.0;
  float var = max(abs(mean2 - mean * mean), 1e-4);
  float p = var / (var + (dist - mean) * (dist - mean));
  return max(p * p * p, 0.0);
}

// Irradiância no ponto P de normal N: as 8 sondas em volta, pesadas pela trilinear, pela validade e por quanto a sonda
// está do lado da frente da superfície (Majercik et al. 2019, "Dynamic Diffuse Global Illumination with Ray-Traced
// Irradiance Fields": o peso "wrap" contra a luz que vem de trás da parede). O ponto sai um pouco da superfície.
vec3 giProbeIrradiance(vec3 P, vec3 N) {
  vec3 o = giHeader[0].xyz;
  float sp = giHeader[0].w;
  ivec3 dims = ivec3(giHeader[1].xyz);
  vec3 Pb = P + N * (0.3 * sp);
  vec3 g = clamp((Pb - o) / sp, vec3(0.0), vec3(dims - 1));
  ivec3 b = clamp(ivec3(floor(g)), ivec3(0), max(dims - 2, ivec3(0)));
  vec3 f = clamp(g - vec3(b), 0.0, 1.0);
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int c = 0; c < 8; ++c) {
    ivec3 off = ivec3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
    ivec3 q = min(b + off, dims - 1);
    uint i = uint((q.z * dims.y + q.y) * dims.x + q.x) * GI_STRIDE;
    float valid = giData[i + 3u].x;
    if (valid <= 0.0) continue;
    vec3 tri = mix(vec3(1.0) - f, f, vec3(off));
    vec3 toProbe = o + vec3(q) * sp - P;
    float d = length(toProbe);
    float wrap = d > 1e-4 ? dot(toProbe / d, N) * 0.5 + 0.5 : 1.0;
    float w = tri.x * tri.y * tri.z * valid * (wrap * wrap + 0.05) * max(giProbeVisibility(i, Pb - (o + vec3(q) * sp)), 1e-3);
    vec3 E = vec3(giData[i].x + dot(giData[i].yzw, N), giData[i + 1u].x + dot(giData[i + 1u].yzw, N),
                  giData[i + 2u].x + dot(giData[i + 2u].yzw, N));
    sum += max(E, vec3(0.0)) * w;
    wsum += w;
  }
  return wsum > 1e-6 ? sum / wsum : vec3(0.0);
}
#endif
