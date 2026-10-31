// Ray queries contra o TLAS da cena (set 0, binding 6), com teste de alfa para folhagem/grades.
#ifndef RAYTRACING_GLSL
#define RAYTRACING_GLSL

#extension GL_EXT_ray_query : require
#include "scene.glsl"
#include "terminator.glsl"

layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;

// Origem do raio afastada da superfície pela normal GEOMÉTRICA (Wächter & Binder, "A Fast and Robust
// Method for Avoiding Self-Intersection", Ray Tracing Gems cap. 6): deslocamento em ULPs do float,
// que escala com a magnitude da coordenada, mais `bias` metros (erro de posição conhecido do chamador).
vec3 offsetRayOrigin(vec3 p, vec3 ng, float bias) {
  p += ng * bias;
  const float originThreshold = 1.0 / 32.0, floatScale = 1.0 / 65536.0, intScale = 256.0;
  ivec3 o = ivec3(intScale * ng);
  vec3 pi = vec3(intBitsToFloat(floatBitsToInt(p.x) + (p.x < 0.0 ? -o.x : o.x)),
                 intBitsToFloat(floatBitsToInt(p.y) + (p.y < 0.0 ? -o.y : o.y)),
                 intBitsToFloat(floatBitsToInt(p.z) + (p.z < 0.0 ? -o.z : o.z)));
  return vec3(abs(p.x) < originThreshold ? p.x + floatScale * ng.x : pi.x,
              abs(p.y) < originThreshold ? p.y + floatScale * ng.y : pi.y,
              abs(p.z) < originThreshold ? p.z + floatScale * ng.z : pi.z);
}

// Candidatos não-opacos (materiais com recorte): aceita só se o alfa da textura passar.
bool alphaTestPasses(uint inst, uint prim, vec2 bary) {
  InstanceData d = instances[inst];
  Material m = materials[d.material];
  if ((m.flags & MATERIAL_ALPHA_MASK) == 0u) return true;
  float a = m.baseColor.a;
  IndexData ib = IndexData(d.indices);
  uint i0 = ib.i[prim * 3u], i1 = ib.i[prim * 3u + 1u], i2 = ib.i[prim * 3u + 2u];
  a *= interpolateVertexColor(d, i0, i1, i2, vec3(1.0 - bary.x - bary.y, bary.x, bary.y)).a;
  if (m.baseColorTex >= 0) {
    VertexData vb = VertexData(d.vertices);
    vec2 uv = fetchVertexUV(vb, i0) * (1.0 - bary.x - bary.y) + fetchVertexUV(vb, i1) * bary.x + fetchVertexUV(vb, i2) * bary.y;
    a *= textureLod(textures[nonuniformEXT(m.baseColorTex)], uv, 0.0).a;
  }
  return a >= m.alphaCutoff;
}

#define RESOLVE_CANDIDATES(rq)                                                                         \
  while (rayQueryProceedEXT(rq)) {                                                                     \
    if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&    \
        alphaTestPasses(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false)),                \
                        uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),                     \
                        rayQueryGetIntersectionBarycentricsEXT(rq, false)))                            \
      rayQueryConfirmIntersectionEXT(rq);                                                              \
  }

// Raios de sombra veem só instâncias com este bit (rhi::TlasInstance::mask): globos de lâmpada e vidro não bloqueiam.
const uint RAY_MASK_SHADOW = 0x01u, RAY_MASK_GLASS = 0x04u;

float traceVisibility(vec3 origin, vec3 dir, float tMax) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, RAY_MASK_SHADOW, origin, 0.0, dir, tMax);
  RESOLVE_CANDIDATES(rq)
  return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// Sombra colorida: depois do teste opaco, um raio que só vê vidro (RAY_MASK_GLASS) percorre as lâminas acumulando a
// transmitância (transmissão × cor × (1 − Fresnel) e a absorção dos volumes). Cenas sem vidro: só o teste opaco.
vec3 glassTransmittanceAlong(vec3 origin, vec3 dir, float tMax);  // lighting.glsl (precisa do material)
vec3 traceTransmittance(vec3 origin, vec3 dir, float tMax) {
  if (traceVisibility(origin, dir, tMax) == 0.0) return vec3(0.0);
#ifdef GLASS_SHADOWS
  return glassTransmittanceAlong(origin, dir, tMax);
#else
  return vec3(1.0);
#endif
}

#ifdef SURFACE_GLSL
#ifdef RESTIR_GLSL
// Visibilidade de um ponto de luz a partir da superfície exata de outro pixel (pesos de MIS do ReSTIR).
float visibleFrom(PixelSurface s, uint li, vec2 uv) {
  vec3 L, Le;
  float dist, G;
  if (!lightPoint(li, uv, s.g.P, L, dist, Le, G) || dot(s.Nb, L) <= 0.0) return 0.0;
  return traceVisibility(offsetRayOrigin(s.Pshadow, s.g.Ng, s.rayBias), L, dist * 0.99999);
}
#endif
#endif

struct HitInfo {
  vec3 P;
  vec3 Pshadow;  // origem dos raios de sombra corrigida para o terminator (Hanika 2021)
  vec3 N;   // normal de shading, virada contra o raio
  vec3 Ng;  // normal geométrica do triângulo, virada contra o raio
  vec2 uv;
  vec4 color;  // cor por vértice interpolada
  uint material;
  float t;
};

bool traceHit(vec3 origin, vec3 dir, float tMax, out HitInfo hit) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, 0xFF, origin, 0.0, dir, tMax);
  RESOLVE_CANDIDATES(rq)
  if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return false;

  uint inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
  uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
  vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
  mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);
  InstanceData d = instances[inst];
  IndexData ib = IndexData(d.indices);
  VertexData vb = VertexData(d.vertices);
  uint i0 = ib.i[prim * 3u], i1 = ib.i[prim * 3u + 1u], i2 = ib.i[prim * 3u + 2u];
  float b0 = 1.0 - bary.x - bary.y;
  mat3 normalMatrix = transpose(inverse(mat3(o2w)));
  vec3 n = fetchVertexNormal(vb, i0) * b0 + fetchVertexNormal(vb, i1) * bary.x + fetchVertexNormal(vb, i2) * bary.y;
  vec3 p0 = fetchVertexPosition(vb, i0), p1 = fetchVertexPosition(vb, i1), p2 = fetchVertexPosition(vb, i2);
  hit.Ng = normalize(normalMatrix * cross(p1 - p0, p2 - p0));
  float side = dot(hit.Ng, dir) > 0.0 ? -1.0 : 1.0;
  hit.Ng *= side;
  hit.N = normalize(normalMatrix * n);
  if (dot(hit.N, hit.Ng) < 0.0) hit.N = -hit.N;  // mesmo lado da faceta
  hit.uv = fetchVertexUV(vb, i0) * b0 + fetchVertexUV(vb, i1) * bary.x + fetchVertexUV(vb, i2) * bary.y;
  hit.color = interpolateVertexColor(d, i0, i1, i2, vec3(b0, bary.x, bary.y));
  hit.t = rayQueryGetIntersectionTEXT(rq, true);
  hit.P = origin + dir * hit.t;
  hit.material = d.material;
  vec3 n0 = normalize(normalMatrix * fetchVertexNormal(vb, i0)) * side;
  vec3 n1 = normalize(normalMatrix * fetchVertexNormal(vb, i1)) * side;
  vec3 n2 = normalize(normalMatrix * fetchVertexNormal(vb, i2)) * side;
  hit.Pshadow = shadowTerminatorPoint(hit.P, o2w * vec4(p0, 1.0), o2w * vec4(p1, 1.0), o2w * vec4(p2, 1.0), n0, n1, n2,
                                      vec3(b0, bary.x, bary.y));
  return true;
}

#endif
