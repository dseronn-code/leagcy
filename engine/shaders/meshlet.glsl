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
struct MeshletLod {  // == scene::MeshletLod
  vec4 self;    // centro xyz, raio
  vec4 parent;
  float selfError, parentError, pad0, pad1;
};
layout(buffer_reference, scalar) readonly buffer MeshletLodList { MeshletLod l[]; };
struct MeshletBlock {  // == scene::MeshletBlock
  vec4 self;
  vec4 parent;
  float minSelfError, maxParentError, pad0, pad1;
};
layout(buffer_reference, scalar) readonly buffer MeshletBlockList { MeshletBlock b[]; };
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
  uint lodBase;     // MeshletLod de cada meshlet, em uints a partir de `meshlets` (0 = malha sem níveis)
  float lodPixels;  // erro máximo na tela, em pixels
  uint blockBase;   // MeshletBlock de cada MESHLET_GROUP meshlets (uints; 0 = sem)
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

// Geometria virtualizada: erro geométrico `err` (unidades da malha) de uma esfera, projetado na tela em pixels, pela
// distância até a borda da esfera (conservador e igual para todos os pedaços que dividem a esfera). Infinito na raiz.
float lodErrorPixels(vec4 sphere, float err, mat4 model, float scale) {
  if (err > 1.0e37) return 1.0e30;
  vec3 c = (model * vec4(sphere.xyz, 1.0)).xyz;
  float d = max(length(c - frame.cameraPos.xyz) - sphere.w * scale, 0.01);
  return err * scale / d * abs(frame.proj[1][1]) * 0.5 * frame.renderSize.y;
}
// Pirâmide de profundidade (o mais distante de cada região; reverse-Z: mínimo). Na fase 0 ela ainda tem a profundidade
// da fase 0 do frame anterior; depois, a deste frame.
layout(set = 1, binding = 0) uniform sampler2D hiZ;

// Esfera (mundo) escondida atrás do que já está na pirâmide, vista por `viewProj` (o mesmo teste do culling por objeto
// em cull.comp, com a caixa da esfera).
bool sphereOccluded(vec3 c, float r, mat4 viewProj) {
  vec2 lo = vec2(1e9), hi = vec2(-1e9);
  float nearest = 0.0;
  for (int k = 0; k < 8; ++k) {
    vec3 p = c + r * vec3((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0, (k & 4) != 0 ? 1.0 : -1.0);
    vec4 q = viewProj * vec4(p, 1.0);
    if (q.w <= 1e-4) return false;  // cruza o plano da câmera
    vec3 n = q.xyz / q.w;
    vec2 uv = n.xy * 0.5 + 0.5;
    lo = min(lo, uv);
    hi = max(hi, uv);
    nearest = max(nearest, n.z);
  }
  lo = clamp(lo, 0.0, 1.0);
  hi = clamp(hi, 0.0, 1.0);
  vec2 size0 = vec2(textureSize(hiZ, 0));
  vec2 extent = (hi - lo) * size0 + 2.0;
  float level = clamp(ceil(log2(max(max(extent.x, extent.y), 1.0))), 0.0, float(textureQueryLevels(hiZ) - 1));
  ivec2 sz = max(ivec2(size0) >> int(level), ivec2(1));
  ivec2 a = clamp(ivec2(floor(lo * vec2(sz))) - 1, ivec2(0), sz - 1), z = clamp(ivec2(floor(hi * vec2(sz))) + 1, ivec2(0), sz - 1);
  float farthest = 1.0;
  for (int y = a.y; y <= z.y; ++y)
    for (int x = a.x; x <= z.x; ++x) farthest = min(farthest, texelFetch(hiZ, ivec2(x, y), int(level)).r);
  return nearest < farthest;
}

// Oclusão por meshlet em duas fases (flags): 4 = fase 0, descarta o que a pirâmide do frame anterior esconde (com as
// matrizes anteriores); 8 = repasse depois da pirâmide nova, desenha só o que a anterior escondia e a nova mostra;
// 16 = fase 1 (objetos novos), descarta o que a pirâmide nova esconde.
bool meshletPassesOcclusion(Meshlet ml, uint object) {
  uint mode = pc.flags & 28u;
  if (mode == 0u) return true;
  ObjectData o = objects[object];
  float scale = max(length(o.model[0].xyz), max(length(o.model[1].xyz), length(o.model[2].xyz)));
  float r = ml.radius * scale;
  if (mode == 16u) return !sphereOccluded((o.model * vec4(ml.center, 1.0)).xyz, r, frame.viewProjNoJitter);
  bool before = sphereOccluded((o.prevModel * vec4(ml.center, 1.0)).xyz, r, frame.prevViewProjNoJitter);
  if (mode == 4u) return !before;
  return before && !sphereOccluded((o.model * vec4(ml.center, 1.0)).xyz, r, frame.viewProjNoJitter);
}

// Esfera fora da tela (4 planos laterais).
bool sphereOutside(vec3 c, float r) {
  mat4 vp = frame.viewProjNoJitter;
  vec4 row0 = vec4(vp[0][0], vp[1][0], vp[2][0], vp[3][0]), row1 = vec4(vp[0][1], vp[1][1], vp[2][1], vp[3][1]),
       row3 = vec4(vp[0][3], vp[1][3], vp[2][3], vp[3][3]);
  vec4 planes[4] = vec4[4](row3 + row0, row3 - row0, row3 + row1, row3 - row1);
  for (int i = 0; i < 4; ++i) {
    vec4 p = planes[i] / length(planes[i].xyz);
    if (dot(p.xyz, c) + p.w < -r) return true;
  }
  return false;
}

// O bloco `b` (MESHLET_GROUP meshlets seguidos) pode ter algum meshlet no corte? Falso se todos são finos demais
// (cota superior do erro dos pais cabe no limite), grossos demais (cota inferior do erro próprio passa dele) ou se o
// bloco está fora da tela. Os limites valem para qualquer meshlet do bloco (esferas que contêm as deles).
bool blockMayDraw(uint b, mat4 model) {
  if (pc.blockBase == 0u) return true;
  MeshletBlock k = MeshletBlockList(pc.meshlets + uint64_t(pc.blockBase) * 4ul).b[b];
  float scale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
  float K = abs(frame.proj[1][1]) * 0.5 * frame.renderSize.y;
  vec3 cs = (model * vec4(k.self.xyz, 1.0)).xyz;
  float rs = k.self.w * scale, ds = length(cs - frame.cameraPos.xyz);
  if (k.minSelfError * scale / max(ds + rs, 0.01) * K > pc.lodPixels) return false;
  if (k.maxParentError < 1.0e37) {
    vec3 cp = (model * vec4(k.parent.xyz, 1.0)).xyz;
    float dp = max(length(cp - frame.cameraPos.xyz) - k.parent.w * scale, 0.01);
    if (k.maxParentError * scale / dp * K <= pc.lodPixels) return false;
  }
  if ((pc.flags & 1u) != 0u && sphereOutside(cs, rs + 2.0 * ds / (abs(frame.proj[1][1]) * frame.renderSize.y))) return false;
  return true;
}

// O meshlet `m` está no corte do grafo: o próprio erro cabe no limite e o do grupo que o substitui, não.
bool meshletInLodCut(uint m, mat4 model) {
  if (pc.lodBase == 0u) return true;
  MeshletLod l = MeshletLodList(pc.meshlets + uint64_t(pc.lodBase) * 4ul).l[m];
  float scale = max(length(model[0].xyz), max(length(model[1].xyz), length(model[2].xyz)));
  return lodErrorPixels(l.self, l.selfError, model, scale) <= pc.lodPixels &&
         lodErrorPixels(l.parent, l.parentError, model, scale) > pc.lodPixels;
}

#endif
