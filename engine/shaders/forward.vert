#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(push_constant) uniform Push {
  mat4 model;
  vec4 albedoRough;
  vec4 emissiveMetal;
  uvec4 flags;  // x = índice da instância, y = tipo de material
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main() {
  vec4 world = pc.model * vec4(aPosition, 1.0);
  vWorldPos = world.xyz;
  vNormal = transpose(inverse(mat3(pc.model))) * aNormal;
  vUV = aUV;
  gl_Position = frame.viewProj * world;
}
