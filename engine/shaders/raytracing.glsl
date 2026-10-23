// Ray queries contra o TLAS da cena (set 0, binding 6), com teste de alfa para folhagem/grades.
#ifndef RAYTRACING_GLSL
#define RAYTRACING_GLSL

#extension GL_EXT_ray_query : require
#include "scene.glsl"

layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;

// Origem do raio afastada da superfície pela normal GEOMÉTRICA (Wächter & Binder, "A Fast and Robust
// Method for Avoiding Self-Intersection", Ray Tracing Gems cap. 6): deslocamento em ULPs do float,
// que escala com a magnitude da coordenada, mais uma margem proporcional à distância da câmera para
// cobrir o erro da reconstrução da posição a partir da profundidade.
vec3 offsetRayOrigin(vec3 p, vec3 ng, float viewDistance) {
  p += ng * (1e-4 * viewDistance);
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
  if (m.baseColorTex >= 0) {
    IndexData ib = IndexData(d.indices);
    VertexData vb = VertexData(d.vertices);
    uint i0 = ib.i[prim * 3u], i1 = ib.i[prim * 3u + 1u], i2 = ib.i[prim * 3u + 2u];
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

float traceVisibility(vec3 origin, vec3 dir, float tMax) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFF, origin, 0.0, dir, tMax);
  RESOLVE_CANDIDATES(rq)
  return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

struct HitInfo {
  vec3 P;
  vec3 N;   // normal de shading, virada contra o raio
  vec3 Ng;  // normal geométrica do triângulo, virada contra o raio
  vec2 uv;
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
  if (dot(hit.Ng, dir) > 0.0) hit.Ng = -hit.Ng;
  hit.N = normalize(normalMatrix * n);
  if (dot(hit.N, hit.Ng) < 0.0) hit.N = -hit.N;  // mesmo lado da faceta
  hit.uv = fetchVertexUV(vb, i0) * b0 + fetchVertexUV(vb, i1) * bary.x + fetchVertexUV(vb, i2) * bary.y;
  hit.t = rayQueryGetIntersectionTEXT(rq, true);
  hit.P = origin + dir * hit.t;
  hit.material = d.material;
  return true;
}

#endif
