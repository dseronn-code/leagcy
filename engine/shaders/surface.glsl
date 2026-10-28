// Superfície exata do pixel a partir do visibility buffer (objeto + triângulo), para os passes que
// lançam raios. Defina SURFACE_VISIBILITY_BINDING (set 1) antes de incluir.
//
// Por que não usar só a profundidade: a posição reconstruída do depth tem erro proporcional à distância
// e a normal geométrica reconstruída erra nas quinas. Com o triângulo em mãos:
//   - P é a interseção exata do raio da câmera com o plano do triângulo;
//   - Ng é a normal da faceta real;
//   - Nb é a normal suave interpolada dos vértices (antes do normal map);
//   - Pshadow é o ponto de saída dos raios de sombra corrigido para o "terminator" (Hanika 2021).
#ifndef SURFACE_GLSL
#define SURFACE_GLSL

#include "gbuffer.glsl"
#include "scene.glsl"
#include "terminator.glsl"

layout(set = 1, binding = SURFACE_VISIBILITY_BINDING) uniform usampler2D gVisibility;

// Margem extra para a origem dos raios (além dos ULPs de Wächter-Binder), em metros por metro de
// distância da câmera: a posição reconstruída do depth erra ~1e-4 × distância; a interseção exata com o
// triângulo erra na ordem do epsilon do float (1e-6 cobre com folga: 1 mm a 1 km).
const float RAY_BIAS_DEPTH = 1e-4, RAY_BIAS_EXACT = 1e-6;

struct PixelSurface {
  GSample g;      // g.P e g.Ng exatos quando o triângulo é conhecido
  float rayBias;  // margem da origem dos raios (m)
  vec3 Nb;        // normal suave interpolada (sem normal map), do lado da câmera
  vec3 Pshadow;   // origem dos raios de sombra (antes do deslocamento de Wächter-Binder)
};

// Interseção exata do raio (C, dir) com o triângulo `prim` do objeto `obj` (transformação `model`).
// Preenche P, Ng, V, Nb, Pshadow e rayBias de `s`; falso se o triângulo está de lado para o raio.
bool intersectTriangle(uint obj, uint prim, mat4 model, vec3 C, vec3 dir, inout PixelSurface s) {
  InstanceData d = instances[obj];
  IndexData ib = IndexData(d.indices);
  VertexData vb = VertexData(d.vertices);
  uint i0 = ib.i[prim * 3u], i1 = ib.i[prim * 3u + 1u], i2 = ib.i[prim * 3u + 2u];
  mat3 normalMatrix = transpose(inverse(mat3(model)));
  vec3 p0 = (model * vec4(fetchVertexPosition(vb, i0), 1.0)).xyz;
  vec3 p1 = (model * vec4(fetchVertexPosition(vb, i1), 1.0)).xyz;
  vec3 p2 = (model * vec4(fetchVertexPosition(vb, i2), 1.0)).xyz;

  vec3 e1 = p1 - p0, e2 = p2 - p0;
  vec3 ng = cross(e1, e2);
  float denom = dot(dir, ng);
  if (abs(denom) < 1e-12 * length(ng)) return false;
  float t = dot(p0 - C, ng) / denom;
  vec3 P = C + dir * t;

  // Baricêntricas do ponto (regra de Cramer nas arestas).
  vec3 q = P - p0;
  float d11 = dot(e1, e1), d12 = dot(e1, e2), d22 = dot(e2, e2), q1 = dot(q, e1), q2 = dot(q, e2);
  float det = d11 * d22 - d12 * d12;
  if (det <= 0.0) return false;
  float b1 = (d22 * q1 - d12 * q2) / det, b2 = (d11 * q2 - d12 * q1) / det;
  vec3 b = clamp(vec3(1.0 - b1 - b2, b1, b2), 0.0, 1.0);
  b /= b.x + b.y + b.z;

  // Normais do lado da câmera (materiais de dois lados mostram o verso).
  vec3 Ng = normalize(ng);
  float side = dot(Ng, C - P) < 0.0 ? -1.0 : 1.0;
  Ng *= side;
  vec3 n0 = normalize(normalMatrix * fetchVertexNormal(vb, i0)) * side;
  vec3 n1 = normalize(normalMatrix * fetchVertexNormal(vb, i1)) * side;
  vec3 n2 = normalize(normalMatrix * fetchVertexNormal(vb, i2)) * side;
  vec3 Nb = normalize(n0 * b.x + n1 * b.y + n2 * b.z);
  if (dot(Nb, Ng) < 0.0) Nb = Ng;  // normais de vértice inconsistentes

  s.g.P = P;
  s.g.Ng = Ng;
  s.g.V = -dir;
  s.Nb = Nb;
  s.Pshadow = shadowTerminatorPoint(P, p0, p1, p2, n0, n1, n2, b);
  s.rayBias = RAY_BIAS_EXACT * t;
  return true;
}

PixelSurface loadSurface(ivec2 id) {
  PixelSurface s;
  s.g = loadGBuffer(id);
  s.Nb = s.g.N;
  s.Pshadow = s.g.P;
  s.rayBias = RAY_BIAS_DEPTH * length(frame.cameraPos.xyz - s.g.P);
  uvec2 vis = texelFetch(gVisibility, id, 0).xy;
  if (vis.x == 0u || s.g.depth <= 0.0) return s;
  // Raio do centro do pixel (mesma projeção com jitter do rasterizador).
  vec3 C = frame.cameraPos.xyz;
  vec3 dir = viewDirFromUv((vec2(id) + 0.5) * frame.renderSize.zw);
  intersectTriangle(vis.x - 1u, vis.y, objects[vis.x - 1u].model, C, dir, s);
  return s;
}

#ifdef SURFACE_PREV_VISIBILITY_BINDING
layout(set = 1, binding = SURFACE_PREV_VISIBILITY_BINDING) uniform usampler2D gVisibilityPrev;

// Superfície exata do pixel `pid` no FRAME ANTERIOR: triângulo do visibility buffer anterior, transformação
// anterior do objeto, câmera e jitter anteriores. g.N/roughness vêm do G-buffer anterior; o material é o de
// `current` (mesma superfície). Falso se não havia geometria ali.
bool loadPreviousSurface(ivec2 pid, sampler2D normalPrev, GSample current, out PixelSurface s) {
  s.g = current;
  s.Nb = current.N;
  s.Pshadow = current.P;
  s.rayBias = 0.0;
  uvec2 vis = texelFetch(gVisibilityPrev, pid, 0).xy;
  if (vis.x == 0u) return false;
  vec3 C = frame.prevCameraPos.xyz;
  vec4 far = frame.prevInvViewProj * vec4(((vec2(pid) + 0.5) * frame.renderSize.zw) * 2.0 - 1.0, 0.5, 1.0);
  vec3 dir = normalize(far.xyz / far.w);  // relativa à câmera anterior
  if (!intersectTriangle(vis.x - 1u, vis.y, objects[vis.x - 1u].prevModel, C, dir, s)) return false;
  vec4 n = texelFetch(normalPrev, pid, 0);
  s.g.N = normalize(n.xyz);
  s.g.roughness = n.w;
  return true;
}
#endif

#endif
