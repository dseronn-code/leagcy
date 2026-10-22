// Shading de superfícies. Com RAY_TRACING definido, usa ray queries para:
//  - sombras suaves do sol (disco solar amostrado)
//  - iluminação global difusa de 1 rebote (raio cosseno-ponderado)
//  - reflexões especulares (amostragem VNDF do GGX)
// O ruído de 1 amostra/pixel converge pela acumulação temporal.
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "brdf.glsl"
#include "material.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
  mat4 model;
  vec4 albedoRough;
  vec4 emissiveMetal;
  uvec4 flags;
} pc;

#ifdef RAY_TRACING
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 3) uniform accelerationStructureEXT tlas;

struct InstanceData {
  uint64_t vertices;
  uint64_t indices;
  vec4 albedoRough;
  vec4 emissiveMetal;
  uvec4 flags;
};
layout(set = 0, binding = 4, std430) readonly buffer Instances { InstanceData instances[]; };
layout(buffer_reference, scalar) readonly buffer VertexData { float v[]; };  // 8 floats por vértice
layout(buffer_reference, scalar) readonly buffer IndexData { uint i[]; };

const float RAY_EPS = 0.02;

float traceVisibility(vec3 origin, vec3 dir, float tMax) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.0, dir, tMax);
  while (rayQueryProceedEXT(rq)) {}
  return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

vec3 fetchNormal(VertexData vd, uint idx) {
  return vec3(vd.v[idx * 8u + 3u], vd.v[idx * 8u + 4u], vd.v[idx * 8u + 5u]);
}

// Traça um raio e devolve a radiância que chega pela direção `dir`.
vec3 traceRadiance(vec3 origin, vec3 dir, inout uint rng) {
  rayQueryEXT rq;
  rayQueryInitializeEXT(rq, tlas, gl_RayFlagsOpaqueEXT, 0xFF, origin, 0.0, dir, 1e4);
  while (rayQueryProceedEXT(rq)) {}
  if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT)
    return sampleSky(dir);

  float t = rayQueryGetIntersectionTEXT(rq, true);
  uint inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
  uint prim = uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true));
  vec2 bary = rayQueryGetIntersectionBarycentricsEXT(rq, true);
  mat4x3 o2w = rayQueryGetIntersectionObjectToWorldEXT(rq, true);

  InstanceData data = instances[inst];
  IndexData ib = IndexData(data.indices);
  VertexData vb = VertexData(data.vertices);
  uint i0 = ib.i[prim * 3u], i1 = ib.i[prim * 3u + 1u], i2 = ib.i[prim * 3u + 2u];
  vec3 nObj = fetchNormal(vb, i0) * (1.0 - bary.x - bary.y) + fetchNormal(vb, i1) * bary.x + fetchNormal(vb, i2) * bary.y;
  vec3 N = normalize(transpose(inverse(mat3(o2w))) * nObj);
  if (dot(N, dir) > 0.0) N = -N;
  vec3 P = origin + dir * t;

  Surface s = evalMaterial(data.albedoRough, data.emissiveMetal, data.flags.y, P, N);
  vec3 L = sampleCone(frame.sunDir.xyz, cos(frame.params.x), rand2(rng));
  float NoL = max(dot(N, L), 0.0);
  vec3 diffuse = s.albedo * (1.0 - s.metallic);
  vec3 direct = vec3(0.0);
  if (NoL > 0.0) direct = diffuse / PI * NoL * sunIrradianceAt(P) * traceVisibility(P + N * RAY_EPS, L, 1e4);
  // Rebotes seguintes aproximados pelo céu (sem oclusão).
  vec3 ambient = diffuse * sampleSky(normalize(N + vec3(0, 1, 0))) * 0.6;
  return direct + ambient + s.emissive;
}
#endif

void main() {
  vec3 P = vWorldPos;
  vec3 V = normalize(frame.cameraPos.xyz - P);
  vec3 N = normalize(vNormal);
  if (dot(N, V) < 0.0) N = -N;  // superfícies de dois lados

  Surface s = evalMaterial(pc.albedoRough, pc.emissiveMetal, pc.flags.y, P, N);
  vec3 diffuseColor = s.albedo * (1.0 - s.metallic);
  vec3 f0 = mix(vec3(0.04), s.albedo, s.metallic);
  float NoV = max(dot(N, V), 1e-4);

  uint rng = (uint(gl_FragCoord.x) * 1973u + uint(gl_FragCoord.y) * 9277u + frame.frameInfo.x * 26699u) | 1u;
  vec3 sunE = sunIrradianceAt(P);

#ifdef RAY_TRACING
  vec3 Lsun = sampleCone(frame.sunDir.xyz, cos(frame.params.x), rand2(rng));
  float shadow = dot(N, Lsun) > 0.0 ? traceVisibility(P + N * RAY_EPS, Lsun, 1e4) : 0.0;
  vec3 color = evalBRDF(N, V, frame.sunDir.xyz, diffuseColor, f0, s.roughness) * sunE * shadow;

  // GI difusa: estimador com amostragem por cosseno => peso = albedo difuso.
  vec3 Fv = F_Schlick(f0, NoV);
  vec3 dirD = cosineSampleHemisphere(N, rand2(rng));
  color += diffuseColor * (1.0 - Fv) * traceRadiance(P + N * RAY_EPS, dirD, rng);

  // Especular: amostra VNDF => peso = F * G1(L).
  float a = max(s.roughness * s.roughness, 0.002);
  vec3 T, B;
  orthonormalBasis(N, T, B);
  vec3 Vt = vec3(dot(V, T), dot(V, B), dot(V, N));
  vec3 Ht = sampleGGXVNDF(Vt, a, rand2(rng));
  vec3 H = T * Ht.x + B * Ht.y + N * Ht.z;
  vec3 Ls = reflect(-V, H);
  float NoLs = dot(N, Ls);
  if (NoLs > 0.0)
    color += traceRadiance(P + N * RAY_EPS, Ls, rng) * F_Schlick(f0, max(dot(V, H), 0.0)) * G1_Smith(NoLs, a);
#else
  vec3 color = evalBRDF(N, V, frame.sunDir.xyz, diffuseColor, f0, s.roughness) * sunE;
  color += diffuseColor * sampleSky(normalize(N + vec3(0, 1, 0))) * 0.8;
  vec3 R = reflect(-V, N);
  color += F_Schlick(f0, NoV) * sampleSky(normalize(mix(R, N, s.roughness * s.roughness))) * (1.0 - s.roughness * 0.7);
#endif

  color += s.emissive;
  color = applyHeightFog(color, P);
  outColor = vec4(color, 1.0);
}
