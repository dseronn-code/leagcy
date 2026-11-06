// ReSTIR passo 2: reuso espacial com MIS em pares (Bitterli 2022; "A Gentle Introduction to ReSTIR",
// Wyman et al. 2023): cada vizinho é pesado contra o pixel central avaliando as amostras nas duas
// superfícies, então vizinhos de geometria diferente não escurecem nem clareiam bordas. O(k).
#extension GL_GOOGLE_include_directive : require
#include "restir.glsl"
#define SURFACE_VISIBILITY_BINDING 6
#include "surface.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 4, std430) readonly buffer InReservoirs { Reservoir inReservoirs[]; };
layout(set = 1, binding = 5, std430) writeonly buffer OutReservoirs { Reservoir outReservoirs[]; };

const float RADIUS = 30.0;      // pixels
const uint MAX_NEIGHBORS = 16u;

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  if (!insideRender(id)) return;
  uint width = uint(frame.renderSize.x);
  uint idx = uint(id.y) * width + uint(id.x);
  PixelSurface sc = loadSurface(id);
  GSample g = sc.g;
  Reservoir c = inReservoirs[idx];
  if (g.depth <= 0.0 || frame.frameInfo.z == 0u) { outReservoirs[idx] = c; return; }

  uint rng = seedFor(uvec2(id), frame.frameInfo.x, 2u);
  BrdfContext bc = brdfContextFor(g);
  float zCur = linearDepth(g.depth);

  // Vizinhos válidos (superfície parecida). Pixel sem histórico (desoclusão) pede o dobro.
  uint wanted = (uint(frame.environment.w) & 2u) != 0u ? 0u : min(frame.quality0.z * (c.M < float(restirCandidates()) * 2.0 ? 2u : 1u), MAX_NEIGHBORS);
  ivec2 neighbors[MAX_NEIGHBORS];
  uint k = 0u;
  for (uint tries = 0u; tries < wanted * 2u && k < wanted; ++tries) {
    vec2 u = rand2(rng);
    float r = RADIUS * sqrt(u.x), phi = TWO_PI * u.y;
    ivec2 q = id + ivec2(round(vec2(cos(phi), sin(phi)) * r));
    if (!insideRender(q) || q == id) continue;
    float dq = texelFetch(gDepth, q, 0).r;
    if (dq <= 0.0 || abs(linearDepth(dq) - zCur) > 0.1 * zCur || dot(texelFetch(gNormal, q, 0).xyz, g.N) < 0.5) continue;
    neighbors[k++] = q;
  }

  float pcXc = targetPdf(g, bc, c.light, c.uv);
  ResamplingState s = beginResampling();
  float mc = k == 0u ? 1.0 : 0.0;
  float invK = k > 0u ? 1.0 / float(k) : 0.0;
  for (uint i = 0u; i < k; ++i) {
    ivec2 q = neighbors[i];
    Reservoir n = inReservoirs[uint(q.y) * width + uint(q.x)];
    PixelSurface sq = loadSurface(q);
    GSample gq = sq.g;
    BrdfContext bcq = brdfContextFor(gq);
    float piXi = targetPdf(gq, bcq, n.light, n.uv), pcXi = targetPdf(g, bc, n.light, n.uv);
    float piXc = targetPdf(gq, bcq, c.light, c.uv);
#ifdef RAY_TRACING
    // Correção de viés por ray tracing: o vizinho descarta pontos de luz ocultos para ele, então só "poderia
    // ter gerado" a amostra central se ela for visível de lá. Sem isto, o pixel central perde peso perto de
    // sombras (escurece).
    if (restirBiasCorrection() && piXc > 0.0) piXc *= visibleFrom(sq, c.light, c.uv);
#endif
    float mi = invK * pairwiseMis(piXi, n.M, pcXi, c.M * invK);
    mc += invK * pairwiseMis(pcXc, c.M * invK, piXc, n.M);
    streamSample(s, n.light, n.uv, mi * pcXi * n.W, pcXi, n.M, rand(rng));
  }
  streamSample(s, c.light, c.uv, mc * pcXc * c.W, pcXc, c.M, rand(rng));
  outReservoirs[idx] = finishResampling(s);
}
