#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"
#define MATERIAL_SAMPLE(i, uv) texture(textures[nonuniformEXT(i)], uv)
#include "material.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vTangent;
layout(location = 4) in vec4 vCurClip;
layout(location = 5) in vec4 vPrevClip;

layout(location = 0) out vec4 outAlbedo;    // rgb = albedo, a = metalicidade
layout(location = 1) out vec4 outNormal;    // xyz = normal (mundo), w = rugosidade
layout(location = 2) out vec2 outMotion;    // deslocamento em UV desde o frame anterior
layout(location = 3) out vec4 outEmissive;

layout(push_constant) uniform Push { uint object; uint material; } pc;

void main() {
  Material m = materials[pc.material];
  vec3 N = normalize(vNormal);
  if (!gl_FrontFacing) N = -N;  // materiais de dois lados
  Surface s = evalMaterial(m, vWorldPos, N, vTangent, vUV);
  if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && s.alpha < m.alphaCutoff) discard;

  // Movimento da câmera + do objeto + da deformação do esqueleto.
  outMotion = (vCurClip.xy / vCurClip.w - vPrevClip.xy / vPrevClip.w) * 0.5;
  outAlbedo = vec4(s.albedo, s.metallic);
  outNormal = vec4(s.normal, s.roughness);
  outEmissive = vec4(clampHalf(s.emissive * preExposure()), 1.0);  // nits, pré-exposto
}
