#version 460
// Recorte por alfa na sombra (folhagem, grades): o resto só escreve profundidade.
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in uint vMaterial;

void main() {
  Material m = materials[vMaterial];
  if ((m.flags & MATERIAL_ALPHA_MASK) != 0u && m.baseColorTex >= 0) {
    float a = m.baseColor.a * texture(textures[nonuniformEXT(m.baseColorTex)], vUV).a;
    if (a < m.alphaCutoff) discard;
  }
}
