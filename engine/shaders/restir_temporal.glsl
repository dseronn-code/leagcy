// ReSTIR passo 1: candidatos iniciais pela BVH de luzes (RIS), visibilidade do escolhido e reuso temporal
// com pesos de MIS generalizados avaliando as amostras na superfície exata do frame anterior; filtro de boiling.
#extension GL_GOOGLE_include_directive : require
#include "restir.glsl"
#define SURFACE_VISIBILITY_BINDING 9
#define SURFACE_PREV_VISIBILITY_BINDING 10
#include "surface.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 4) uniform sampler2D gDepthPrev;
layout(set = 1, binding = 5) uniform sampler2D gNormalPrev;
layout(set = 1, binding = 6) uniform sampler2D gMotion;
layout(set = 1, binding = 7, std430) readonly buffer PrevReservoirs { Reservoir prevReservoirs[]; };
layout(set = 1, binding = 8, std430) writeonly buffer OutReservoirs { Reservoir outReservoirs[]; };

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  bool working = insideRender(id);
  uint idx = uint(id.y) * uint(frame.renderSize.x) + uint(id.x);
  PixelSurface sf;
  GSample g;
  if (working) {
    sf = loadSurface(id);
    g = sf.g;
  }
  uint lightCount = frame.frameInfo.z;
  working = working && g.depth > 0.0 && lightCount > 0u;

  Reservoir r = emptyReservoir();
  if (working) {
    uint rng = seedFor(uvec2(id), frame.frameInfo.x, 1u);
    BrdfContext bc = brdfContextFor(g);

    // 1) RIS: candidatos pela BVH de luzes (pdf exata) e um ponto do cone visível de cada luz.
    uint C = restirCandidates();
    ResamplingState s = beginResampling();
    for (uint i = 0u; i < C; ++i) {
      uint li;
      float pmf, pdfArea;
      vec2 uv;
      if (!sampleLightBvh(g.P, g.N, rng, li, pmf) || !sampleLightPoint(li, g.P, rand2(rng), uv, pdfArea)) {
        s.r.M += 1.0;
        continue;
      }
      float p = targetPdf(g, bc, li, uv);
      streamSample(s, li, uv, p / (float(C) * pmf * pdfArea), p, 1.0, rand(rng));
    }
    r = finishResampling(s);

#ifdef RAY_TRACING
    // 2) Visibilidade do ponto escolhido (o mesmo que será sombreado): oculto => contribuição zero.
    if (r.light != NO_LIGHT) {
      vec3 kd, ks, L;
      float dist;
      lightContribution(g, bc, r.light, r.uv, kd, ks, L, dist);
      vec3 origin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
      if (dot(sf.Nb, L) <= 0.0 || traceVisibility(origin, L, dist * 0.99999) == 0.0) {
        r.light = NO_LIGHT;
        r.W = r.pHat = 0.0;
      }
    }
#endif

    // 3) Reuso temporal: a amostra do frame anterior entra com peso de MIS (balance heuristic com
    //    confiança M) avaliando as duas amostras nas duas superfícies — sem viés em bordas e movimento.
    if (historyValid() && (uint(frame.environment.w) & 1u) == 0u) {
      vec2 prevUv = (vec2(id) + 0.5) * frame.renderSize.zw - texelFetch(gMotion, id, 0).xy;
      ivec2 pid = ivec2(prevUv * frame.renderSize.xy);
      PixelSurface sp;
      if (insideRender(pid) && loadPreviousSurface(pid, gNormalPrev, g, sp)) {
        GSample gp = sp.g;
        float zPrev = length(gp.P - frame.prevCameraPos.xyz), zCur = length(g.P - frame.cameraPos.xyz);
        Reservoir p = prevReservoirs[uint(pid.y) * uint(frame.renderSize.x) + uint(pid.x)];
        if (p.light != NO_LIGHT && p.light >= lightCount) p = emptyReservoir();
        if (abs(zPrev - zCur) < 0.1 * zCur && dot(gp.N, g.N) > 0.5 && p.M > 0.0) {
          p.M = min(p.M, RESTIR_MAX_HISTORY * float(C));
          BrdfContext bcp = brdfContextFor(gp);
          float pcXc = r.pHat, ppXc = targetPdf(gp, bcp, r.light, r.uv);
#ifdef RAY_TRACING
          // Correção de viés por ray tracing: a visibilidade muda entre frames (jitter do TAA, objetos que se
          // movem); o histórico só "poderia ter gerado" a amostra atual se ela fosse visível da superfície
          // anterior. Sem isto o histórico leva crédito por pontos que nunca teria e a imagem escurece.
          if (restirBiasCorrection() && ppXc > 0.0) ppXc *= visibleFrom(sp, r.light, r.uv);
#endif
          float pcXp = targetPdf(g, bc, p.light, p.uv), ppXp = targetPdf(gp, bcp, p.light, p.uv);
#ifdef RAY_TRACING
          // A amostra do histórico foi verificada do ponto anterior; com o jitter/movimento ela pode estar
          // oculta agora. Retestada, todo reservatório que sai daqui é visível do ponto atual — a premissa
          // da correção de viés no reuso espacial.
          if (restirBiasCorrection() && pcXp > 0.0 && p.W > 0.0 && visibleFrom(sf, p.light, p.uv) == 0.0) p.W = 0.0;
#endif
          ResamplingState t = beginResampling();
          streamSample(t, r.light, r.uv, pairwiseMis(pcXc, r.M, ppXc, p.M) * pcXc * r.W, pcXc, r.M, rand(rng));
          streamSample(t, p.light, p.uv, pairwiseMis(ppXp, p.M, pcXp, r.M) * pcXp * p.W, pcXp, p.M, rand(rng));
          r = finishResampling(t);
        }
      }
    }
  }

  // O filtro troca viés (descarta amostras fortes) por estabilidade: fica fora da referência (--reference-pt), que
  // tem que ser a verdade. Condição uniforme no despacho: as barreiras dentro dele continuam válidas.
  if (!pathReference()) boilingFilter(gl_LocalInvocationIndex, r);
  if (insideRender(id)) outReservoirs[idx] = r;
}
