// Ray queries contra o TLAS da cena (set 0, binding 6), com teste de alfa para folhagem/grades.
#ifndef RAYTRACING_GLSL
#define RAYTRACING_GLSL

#extension GL_EXT_ray_query : require
#include "scene.glsl"

layout(set = 0, binding = 6) uniform accelerationStructureEXT tlas;

const float RAY_EPS = 0.01;

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
  vec3 N;   // virada contra o raio
  vec2 uv;
  uint material;
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
  vec3 n = fetchVertexNormal(vb, i0) * b0 + fetchVertexNormal(vb, i1) * bary.x + fetchVertexNormal(vb, i2) * bary.y;
  hit.N = normalize(transpose(inverse(mat3(o2w))) * n);
  if (dot(hit.N, dir) > 0.0) hit.N = -hit.N;
  hit.uv = fetchVertexUV(vb, i0) * b0 + fetchVertexUV(vb, i1) * bary.x + fetchVertexUV(vb, i2) * bary.y;
  hit.P = origin + dir * rayQueryGetIntersectionTEXT(rq, true);
  hit.material = d.material;
  return true;
}

#endif
