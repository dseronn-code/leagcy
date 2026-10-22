#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;

layout(push_constant) uniform Push { mat4 model; uint material; } pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vTangent;

void main() {
  vec4 world = pc.model * vec4(aPosition, 1.0);
  mat3 normalMatrix = transpose(inverse(mat3(pc.model)));
  vWorldPos = world.xyz;
  vNormal = normalMatrix * aNormal;
  vTangent = vec4(mat3(pc.model) * aTangent.xyz, aTangent.w);
  vUV = aUV;
  gl_Position = frame.viewProj * world;
}
