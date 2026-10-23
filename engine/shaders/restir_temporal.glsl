// ReSTIR passo 1: candidatos iniciais (RIS) + teste de visibilidade + reuso temporal.
#extension GL_GOOGLE_include_directive : require
#include "restir.glsl"
#define SURFACE_VISIBILITY_BINDING 9
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
  if (!insideRender(id)) return;
  uint idx = uint(id.y) * uint(frame.renderSize.x) + uint(id.x);
  PixelSurface sf = loadSurface(id);
  GSample g = sf.g;
  uint lightCount = frame.frameInfo.z;
  if (g.depth <= 0.0 || lightCount == 0u) { outReservoirs[idx] = emptyReservoir(); return; }

  uint rng = seedFor(uvec2(id), frame.frameInfo.x, 1u);
  BrdfContext bc = brdfContextFor(g);

  // 1) RIS: escolhe entre candidatos uniformes proporcionalmente a p̂.
  Reservoir r = emptyReservoir();
  for (uint i = 0u; i < restirCandidates(); ++i) {
    uint li = min(uint(rand(rng) * float(lightCount)), lightCount - 1u);
    updateReservoir(r, li, targetPdf(li, g, bc) * float(lightCount), 1.0, rand(rng));
  }
  finalizeReservoir(r, g, bc);

#ifdef RAY_TRACING
  // 2) Visibilidade: amostra ocluída não deve se espalhar para os vizinhos.
  if (r.W > 0.0) {
    PointLight l = lights[r.light];
    vec3 Lv = l.position - g.P;
    float dist = length(Lv);
    vec3 origin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
    if (dist > l.radius && (dot(sf.Nb, Lv) <= 0.0 || traceVisibility(origin, Lv / dist, dist - l.radius) == 0.0)) r.W = 0.0;
  }
#endif

  // 3) Reuso temporal: combina com o reservatório do mesmo ponto no frame anterior.
  if (historyValid()) {
    vec2 uv = (vec2(id) + 0.5) * frame.renderSize.zw;
    vec2 prevUv = uv - texelFetch(gMotion, id, 0).xy;
    ivec2 pid = ivec2(prevUv * frame.renderSize.xy);
    if (insideRender(pid)) {
      float zPrev = linearDepth(texelFetch(gDepthPrev, pid, 0).r);
      float zCur = linearDepth(g.depth);
      vec3 nPrev = texelFetch(gNormalPrev, pid, 0).xyz;
      if (abs(zPrev - zCur) < 0.1 * zCur && dot(nPrev, g.N) > 0.9) {
        Reservoir p = prevReservoirs[uint(pid.y) * uint(frame.renderSize.x) + uint(pid.x)];
        p.M = min(p.M, RESTIR_MAX_HISTORY * float(restirCandidates()));
        Reservoir s = emptyReservoir();
        updateReservoir(s, r.light, targetPdf(r.light, g, bc) * r.W * r.M, r.M, rand(rng));
        updateReservoir(s, p.light, targetPdf(p.light, g, bc) * p.W * p.M, p.M, rand(rng));
        finalizeReservoir(s, g, bc);
        r = s;
      }
    }
  }
  outReservoirs[idx] = r;
}
