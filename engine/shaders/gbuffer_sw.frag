#version 460
// Resolução da rasterização por software (sw_raster.comp): passada de tela cheia sobre o G-buffer já desenhado. Onde um
// triângulo por software ficou mais perto (o teste de profundidade decide, com gl_FragDepth = a profundidade dele), o
// pixel ganha as mesmas saídas de gbuffer.frag: baricêntricas com correção de perspectiva no centro do pixel e nos
// vizinhos (gradientes de UV analíticos para a filtragem das texturas, sem depender de quem está no quadrado 2x2).
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#include "frame.glsl"
#include "scene.glsl"

layout(buffer_reference, scalar) readonly buffer SwVis { uint64_t v[]; };
layout(buffer_reference, scalar) readonly buffer SwListR { uint count, pad0, pad1, pad2; uvec2 entries[]; };
layout(buffer_reference, scalar) readonly buffer Words { uint w[]; };
layout(push_constant) uniform Push {
  uint64_t vis;
  uint64_t list;
  uint maxEntries;
  uint pad;
} pc;

vec2 gUVdx = vec2(0.0), gUVdy = vec2(0.0);  // gradientes de UV do pixel (analíticos)
vec4 sampleSoftware(int i, vec2 uv) {
  float s = exp2(textureMipBias());
  return textureGrad(textures[nonuniformEXT(i)], uv, gUVdx * s, gUVdy * s);
}
#define MATERIAL_SAMPLE(i, uv) sampleSoftware(i, uv)
#define TERRAIN_SAMPLE(i, uv) sampleSoftware(i, uv)
#define TERRAIN_REQUEST(i, uv)
#define MATERIAL_DERIVATIVES
#include "material.glsl"

layout(location = 0) out vec4 outDiffuse;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec2 outMotion;
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out vec4 outSpecular;
layout(location = 5) out uvec2 outVisibility;

// O mesmo de gbuffer.frag (Tokuyoshi & Kaplanyan 2019).
float filterRoughness(vec3 normal, float roughness, float normalVariance) {
  const float SIGMA2 = 0.15915494;
  const float KAPPA = 0.18;
  vec3 dndu = dFdx(normal), dndv = dFdy(normal);
  float kernel = min(2.0 * SIGMA2 * (dot(dndu, dndu) + dot(dndv, dndv)), KAPPA);
  float a = roughness * roughness;
  float a2 = clamp(a * a + kernel + normalVariance, 0.0, 1.0);
  return sqrt(sqrt(a2));
}

vec4 clipOf(mat4 model, vec3 pos) {
  vec3 rel = mat3(model) * pos + (model[3].xyz - frame.cameraPos.xyz);
  return frame.proj * vec4(mat3(frame.view) * rel, 1.0);
}

// Baricêntricas com correção de perspectiva no ponto `p` (pixels) de um triângulo com vértices na tela s0..s2 (x, y em
// pixels; w do clip).
vec3 baryAt(vec2 p, vec3 s0, vec3 s1, vec3 s2) {
  float area = (s1.x - s0.x) * (s2.y - s0.y) - (s1.y - s0.y) * (s2.x - s0.x);
  vec3 l = vec3((s2.x - s1.x) * (p.y - s1.y) - (s2.y - s1.y) * (p.x - s1.x),
                (s0.x - s2.x) * (p.y - s2.y) - (s0.y - s2.y) * (p.x - s2.x),
                (s1.x - s0.x) * (p.y - s0.y) - (s1.y - s0.y) * (p.x - s0.x)) / area;
  vec3 b = l / vec3(s0.z, s1.z, s2.z);
  return b / (b.x + b.y + b.z);
}

void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  uint64_t v = SwVis(pc.vis).v[uint(px.y) * uint(frame.renderSize.x) + uint(px.x)];
  if (v == 0ul) discard;
  uint low = uint(v);
  uint e = low >> 7, t = low & 127u;
  if (e >= pc.maxEntries) discard;
  uvec2 en = SwListR(pc.list).entries[e];
  uint object = en.x, mi = en.y;
  InstanceData d = instances[object];
  ObjectData o = objects[object];
  Words w = Words(d.meshlets);
  uint vOff = w.w[mi * 12u + 8u], tOff = w.w[mi * 12u + 9u], firstTriangle = w.w[mi * 12u + 11u];
  uint packed = w.w[d.meshletTriangleBase + tOff + t];
  uint i0 = w.w[d.meshletVertexBase + vOff + (packed & 0xFFu)];
  uint i1 = w.w[d.meshletVertexBase + vOff + ((packed >> 8) & 0xFFu)];
  uint i2 = w.w[d.meshletVertexBase + vOff + ((packed >> 16) & 0xFFu)];
  VertexData vb = VertexData(d.vertices);
  vec3 p0 = fetchVertexPosition(vb, i0), p1 = fetchVertexPosition(vb, i1), p2 = fetchVertexPosition(vb, i2);
  vec4 c0 = clipOf(o.model, p0), c1 = clipOf(o.model, p1), c2 = clipOf(o.model, p2);
  vec2 R = frame.renderSize.xy;
  vec3 s0 = vec3((c0.xy / c0.w * 0.5 + 0.5) * R, c0.w), s1 = vec3((c1.xy / c1.w * 0.5 + 0.5) * R, c1.w),
       s2 = vec3((c2.xy / c2.w * 0.5 + 0.5) * R, c2.w);
  vec2 center = vec2(px) + 0.5;
  vec3 B = baryAt(center, s0, s1, s2), Bx = baryAt(center + vec2(1.0, 0.0), s0, s1, s2),
       By = baryAt(center + vec2(0.0, 1.0), s0, s1, s2);

  vec2 uv0 = fetchVertexUV(vb, i0), uv1 = fetchVertexUV(vb, i1), uv2 = fetchVertexUV(vb, i2);
  vec2 uv = uv0 * B.x + uv1 * B.y + uv2 * B.z;
  gUVdx = (uv0 * Bx.x + uv1 * Bx.y + uv2 * Bx.z) - uv;
  gUVdy = (uv0 * By.x + uv1 * By.y + uv2 * By.z) - uv;
  vec3 pos = p0 * B.x + p1 * B.y + p2 * B.z;
  vec3 worldPos = (o.model * vec4(pos, 1.0)).xyz;
  mat3 normalMatrix = transpose(inverse(mat3(o.model)));
  vec3 normal = normalMatrix * (fetchVertexNormal(vb, i0) * B.x + fetchVertexNormal(vb, i1) * B.y + fetchVertexNormal(vb, i2) * B.z);
  vec4 t0 = vec4(vb.v[i0 * 12u + 8u], vb.v[i0 * 12u + 9u], vb.v[i0 * 12u + 10u], vb.v[i0 * 12u + 11u]);
  vec4 t1 = vec4(vb.v[i1 * 12u + 8u], vb.v[i1 * 12u + 9u], vb.v[i1 * 12u + 10u], vb.v[i1 * 12u + 11u]);
  vec4 t2 = vec4(vb.v[i2 * 12u + 8u], vb.v[i2 * 12u + 9u], vb.v[i2 * 12u + 10u], vb.v[i2 * 12u + 11u]);
  vec4 tangent = t0 * B.x + t1 * B.y + t2 * B.z;
  tangent = vec4(mat3(o.model) * tangent.xyz, t0.w);
  vec4 color = interpolateVertexColor(d, i0, i1, i2, B);
  vec4 curClip = frame.viewProjNoJitter * vec4(worldPos, 1.0);
  vec4 prevClip = frame.prevViewProjNoJitter * (o.prevModel * vec4(pos, 1.0));

  Material m = materials[d.material];
  vec3 N = normalize(normal);
  // Normal do lado visível da faceta (a mesma regra de gbuffer.frag, com a faceta exata do triângulo).
  vec3 w0 = (o.model * vec4(p0, 1.0)).xyz, w1 = (o.model * vec4(p1, 1.0)).xyz, w2 = (o.model * vec4(p2, 1.0)).xyz;
  vec3 Nf = cross(w1 - w0, w2 - w0);
  if (dot(Nf, Nf) > 1e-24) {
    if (dot(Nf, frame.cameraPos.xyz - worldPos) < 0.0) Nf = -Nf;
    if (dot(N, Nf) < 0.0) N = -N;
  }
  Surface s = evalMaterial(m, worldPos, N, tangent, uv, color);

  gl_FragDepth = uintBitsToFloat(uint(v >> 32));
  outMotion = (curClip.xy / curClip.w - prevClip.xy / prevClip.w) * 0.5;
  outDiffuse = vec4(s.diffuseColor, s.occlusion);
  float gRoughness = s.clearcoat >= 0.5 ? filterRoughness(normalize(N), s.clearcoatRoughness, 0.0)
                                        : filterRoughness(s.normal, s.roughness, s.normalVariance);
  outNormal = vec4(s.normal, gRoughness);
  outEmissive = vec4(clampHalf(s.emissive * preExposure()), 1.0);
  outSpecular = vec4(s.f0, s.f90);
  uint prim = firstTriangle == 0xFFFFFFFFu ? (0x80000000u | (mi << 7) | t) : firstTriangle + t;
  outVisibility = uvec2(object + 1u, prim);
}
