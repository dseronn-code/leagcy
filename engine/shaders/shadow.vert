#version 460
// Profundidade vista do sol para uma cascata (shadow maps em cascata: sombra do sol sem ray tracing). Mesmas
// posições do G-buffer (inclusive o vento na folhagem); o mar não projeta sombra.
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in vec3 aPrevPosition;

layout(buffer_reference, scalar) readonly buffer DrawList { uint objects[]; };
layout(push_constant) uniform Push { uint64_t drawList; uint cascade; uint pad; } pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out uint vMaterial;
layout(location = 2) out float vAlpha;  // alfa da cor por vértice (COLOR_0)

vec3 windSway(vec3 p, float height, float t);  // gbuffer.vert (mesma fórmula, copiada abaixo)

void main() {
  uint object = DrawList(pc.drawList).objects[gl_InstanceIndex];
  vMaterial = instances[object].material;
  vUV = aUV;
  vAlpha = fetchVertexColor(instances[object], uint(gl_VertexIndex)).a;
  Material m = materials[vMaterial];
  if ((m.flags & MATERIAL_WATER) != 0u) {  // fora do volume de recorte: não desenha
    gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
    return;
  }
  ObjectData o = objects[object];
  vec3 position = aPosition;
  if (o.anim.x != 0xFFFFFFFFu) position = fetchVertexPosition(VertexData(crowdFrames[o.anim.x + o.anim.y]), uint(gl_VertexIndex));
  vec4 world = o.model * vec4(position, 1.0);
  if ((m.flags & MATERIAL_FOLIAGE) != 0u) world.xyz += windSway(world.xyz, max(world.y - o.model[3].y, 0.0), frame.night.w);
  gl_Position = frame.shadowMatrices[pc.cascade] * world;
}

vec3 windSway(vec3 p, float height, float t) {
  const vec3 WIND = vec3(0.94, 0.0, 0.34);
  float gust = 0.6 + 0.4 * sin(t * 0.7 - dot(p.xz, WIND.xz) * 0.08);
  float sway = (sin(t * 1.4 - dot(p.xz, WIND.xz) * 0.25) * 0.7 + sin(t * 2.9 + p.x * 0.6 + p.z * 0.4) * 0.3) * gust;
  float bend = 0.02 * pow(min(height, 12.0), 1.4);
  float flutter = 0.015 * sin(t * 11.0 + dot(p, vec3(13.1, 7.7, 9.3)));
  return WIND * (bend * (0.6 + sway)) + vec3(flutter, flutter * 0.6, -flutter * 0.4) * min(height, 2.0);
}
