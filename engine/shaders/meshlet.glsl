// Meshlets do G-buffer (gbuffer.task / gbuffer.mesh): a estrutura de scene::Meshlet, as constantes do desenho e o teste
// de visibilidade por meshlet. Os meshlets cobrem só malhas rígidas comuns (sem vento, cabelo, mar, multidão nem
// esqueleto): o resto continua no desenho indexado de gbuffer.vert.
#ifndef MESHLET_GLSL
#define MESHLET_GLSL

#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "frame.glsl"
#include "scene.glsl"

const uint MESHLET_GROUP = 32u;  // meshlets testados por grupo do task shader

struct Meshlet {  // == scene::Meshlet
  vec3 center;
  float radius;
  vec3 coneAxis;
  float coneCutoff;
  uint vertexOffset, triangleOffset, counts, firstTriangle;
};
struct DrawCommand { uint indexCount, instanceCount, firstIndex; int vertexOffset; uint firstInstance; };

layout(buffer_reference, scalar) readonly buffer MeshletList { Meshlet m[]; };
layout(buffer_reference, scalar) readonly buffer MeshletWords { uint w[]; };
layout(buffer_reference, scalar) readonly buffer DrawCommands { DrawCommand c[]; };
layout(buffer_reference, scalar) readonly buffer DrawList { uint objects[]; };

layout(push_constant) uniform Push {
  uint64_t drawList;  // lista visível compactada (cull.comp)
  uint64_t commands;  // comandos indiretos do culling (instanceCount, firstInstance) deste conjunto
  uint64_t meshlets;  // GpuMesh::meshlets
  uint64_t vertices;  // vértices da malha (scene::Vertex, 12 floats)
  uint batch, meshletCount, vertexBase, triangleBase;
  uint flags;  // 1 = culling ligado, 2 = dos dois lados (sem teste de costas)
} pc;

// O que o task shader passa a cada grupo do mesh shader: a entrada da lista (objeto | nível << 30) e os meshlets que
// sobreviveram, na ordem original (a ordem de rasterização fica a mesma do desenho indexado).
struct MeshletTask {
  uint entry;
  uint meshlets[MESHLET_GROUP];
};

// Fora da tela (4 planos laterais, com 1 pixel de folga pelo jitter do TAA) ou com todos os triângulos de costas
// para a câmera (cone das normais, só com escala uniforme: o cone não sobrevive a esticar um eixo).
bool meshletVisible(Meshlet ml, mat4 model) {
  if ((pc.flags & 1u) == 0u) return true;
  vec3 c = (model * vec4(ml.center, 1.0)).xyz;
  float s0 = length(model[0].xyz), s1 = length(model[1].xyz), s2 = length(model[2].xyz);
  float r = ml.radius * max(s0, max(s1, s2));
  vec3 toC = c - frame.cameraPos.xyz;
  float dist = length(toC);
  r += 2.0 * dist / (abs(frame.proj[1][1]) * frame.renderSize.y);
  mat4 vp = frame.viewProjNoJitter;
  vec4 row0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]), row1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]),
       row3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
  vec4 planes[4] = vec4[4](row3 + row0, row3 - row0, row3 + row1, row3 - row1);
  for (int i = 0; i < 4; ++i) {
    vec4 p = planes[i] / length(planes[i].xyz);
    if (dot(p.xyz, c) + p.w < -r) return false;
  }
  if ((pc.flags & 2u) == 0u && ml.coneCutoff < 1.0) {
    vec3 a0 = model[0].xyz / s0, a1 = model[1].xyz / s1, a2 = model[2].xyz / s2;
    bool uniformScale = abs(s0 - s1) < 1e-3 * s0 && abs(s0 - s2) < 1e-3 * s0 && abs(dot(a0, a1)) < 1e-3 &&
                        abs(dot(a0, a2)) < 1e-3 && abs(dot(a1, a2)) < 1e-3;
    // Semelhança (rotação, escala uniforme, espelho): a matriz das normais é proporcional a mat3(model), com fator
    // positivo — o espelho também troca o sentido da frente no rasterizador, então frente e verso continuam os mesmos.
    vec3 axis = normalize(mat3(model) * ml.coneAxis);
    if (uniformScale && dot(toC, axis) >= ml.coneCutoff * dist + r) return false;
  }
  return true;
}

#endif
