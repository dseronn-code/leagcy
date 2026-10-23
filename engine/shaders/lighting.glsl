// Iluminação deferred em compute. Gera dois sinais ruidosos para o denoiser:
//   difuso (demodulado: sem albedo, preserva textura) e especular.
// Com RAY_TRACING: sombras do sol, luz do ReSTIR com sombra, GI de 1 rebote e reflexões.
// Toda radiância sai pré-exposta (ver frame.glsl).
#extension GL_GOOGLE_include_directive : require
#include "restir.glsl"
#define SURFACE_VISIBILITY_BINDING 7
#include "surface.glsl"
#define MATERIAL_SAMPLE(i, uv) textureLod(textures[nonuniformEXT(i)], uv, frame.quality1.x)
#include "material.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 4, std430) readonly buffer Reservoirs { Reservoir reservoirs[]; };
layout(set = 1, binding = 5, rgba16f) uniform writeonly image2D outDiffuse;
layout(set = 1, binding = 6, rgba16f) uniform writeonly image2D outSpecular;

// Dimensões da sequência de Sobol por pixel (cada uma é um par 2D independente).
const uint DIM_SUN = 0u, DIM_LIGHT = 1u, DIM_DIFFUSE = 2u, DIM_SPECULAR = 3u;
const uint DIM_HIT_SUN = 4u, DIM_HIT_LIGHT = 5u, DIM_HIT_LIGHT_PICK = 6u;

// Limita a luminância de amostras indiretas: troca um viés pequeno por muito menos "vagalumes"
// (amostras raras e muito brilhantes que o denoiser não consegue espalhar). Valor pré-exposto
// definido pelo preset (16 ≈ 90x o cinza médio na tela); mais amostras permitem limite mais alto.
vec3 clampFirefly(vec3 c) {
  float limit = frame.quality1.y;
  float l = luminance(c);
  return l > limit ? c * (limit / l) : c;
}

#ifdef RAY_TRACING
// Radiância que sai do ponto atingido em direção à origem do raio: BRDF completo (difuso + especular,
// com compensação de energia) para o sol e uma luz local, reflexo do céu e emissão.
// `bounce` separa as dimensões de Sobol do raio difuso e do especular.
vec3 traceRadiance(vec3 origin, vec3 dir, SampleGen sg, uint bounce) {
  HitInfo hit;
  if (!traceHit(origin, dir, 1e4, hit)) return sampleSky(dir);
  Material m = materials[hit.material];
  Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv);
  BrdfContext bc = makeBrdfContext(hit.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
  vec3 radiance = s.emissive * preExposure();
  uint dimBase = bounce * 3u;
  vec3 kd, ks;

  // Ponto do raio: erro da ordem do epsilon do float × distância percorrida.
  vec3 shadowOrigin2 = offsetRayOrigin(hit.Pshadow, hit.Ng, RAY_BIAS_EXACT * hit.t);
  vec3 sunE = sunIrradianceAt(hit.P);
  if (dot(sunE, sunE) > 0.0) {
    vec3 Ls = sampleCone(frame.sunDir.xyz, cos(frame.params.x), sample2D(sg, DIM_HIT_SUN + dimBase));
    if (dot(hit.N, Ls) > 0.0) {
      evalBRDF(bc, Ls, sqrt(min(s.roughness * s.roughness + 0.5 * frame.params.x, 1.0)), kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0) radiance += (kd * s.diffuseColor + ks) * sunE * traceVisibility(shadowOrigin2, Ls, 1e4);
    }
  }

  uint lightCount = frame.frameInfo.z;
  if (lightCount > 0u) {
    float pick = sample2D(sg, DIM_HIT_LIGHT_PICK + dimBase).x;
    PointLight l = lights[min(uint(pick * float(lightCount)), lightCount - 1u)];
    vec3 L, E;
    float tSurface;
    if (sampleSphereLight(l, hit.P, sample2D(sg, DIM_HIT_LIGHT + dimBase), L, tSurface, E) && dot(hit.N, L) > 0.0) {
      evalBRDF(bc, L, -1.0, kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0 && dot(E, E) > 0.0)
        radiance += (kd * s.diffuseColor + ks) * E * preExposure() * float(lightCount) * traceVisibility(shadowOrigin2, L, tSurface);
    }
  }
  // Ambiente nos rebotes seguintes (sem oclusão — auditoria 3.1): difuso pelo céu acima da normal e
  // especular pelo céu na direção de reflexo (metais deixam de ficar pretos nos reflexos).
  radiance += s.diffuseColor * bc.diffuseWeight * sampleSky(normalize(hit.N + vec3(0, 1, 0))) * 0.5;
  radiance += specularAlbedo(s.f0, s.f90, bc.dfg) * bc.energyCompensation * sampleSky(reflect(dir, hit.N));
  return clampFirefly(radiance);
}
#endif

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  if (!insideRender(id)) return;
  PixelSurface sf = loadSurface(id);
  GSample g = sf.g;
  if (g.depth <= 0.0) {
    imageStore(outDiffuse, id, vec4(0.0));
    imageStore(outSpecular, id, vec4(0.0));
    return;
  }
  vec3 diffuse = vec3(0.0), specular = vec3(0.0), kd, ks;
#ifdef RAY_TRACING
  // Raios de GI/reflexo saem do ponto exato; raios de sombra, do ponto corrigido para o terminator.
  vec3 origin = offsetRayOrigin(g.P, g.Ng, sf.rayBias);
  vec3 shadowOrigin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
#endif
  Reservoir r = reservoirs[uint(id.y) * uint(frame.renderSize.x) + uint(id.x)];
  vec3 sunE = sunIrradianceAt(g.P);
  BrdfContext bc = brdfContextFor(g);

  // Sol como disco de verdade: o BRDF usa a direção central com a rugosidade alargada pelo tamanho
  // angular do disco (Karis 2013) — sem o pico de um sol pontual. É determinístico: fica fora do laço.
  // O termo de relevo (Chiang 2019) suaviza o terminator do normal map.
  vec3 sunKd = vec3(0.0), sunKs = vec3(0.0);
  if (dot(sunE, sunE) > 0.0) {
    evalBRDF(bc, frame.sunDir.xyz, sqrt(min(g.roughness * g.roughness + 0.5 * frame.params.x, 1.0)), sunKd, sunKs);
    float bump = bumpShadowing(sf.Nb, g.N, frame.sunDir.xyz);
    sunKd *= bump;
    sunKs *= bump;
  }

  // Amostras por pixel (preset de qualidade): a sequência de Sobol do pixel avança spp posições por
  // frame, então as amostras do mesmo frame e de frames seguintes continuam estratificadas entre si.
  uint spp = max(frame.quality0.x, 1u);
  float sunVisSum = 0.0;  // só para as visualizações de depuração
  for (uint k = 0u; k < spp; ++k) {
    SampleGen sg = makeSampleGen(uvec2(id), frame.frameInfo.x * spp + k);

    // Sol: só a visibilidade é sorteada no disco (penumbra física).
    if (dot(sunE, sunE) > 0.0) {
#ifdef RAY_TRACING
      vec3 L = sampleCone(frame.sunDir.xyz, cos(frame.params.x), sample2D(sg, DIM_SUN));
      float vis = dot(sf.Nb, L) > 0.0 ? traceVisibility(shadowOrigin, L, 1e4) : 0.0;
#else
      float vis = 1.0;
#endif
      sunVisSum += vis;
      diffuse += sunKd * sunE * vis;
      specular += sunKs * sunE * vis;
    }

    // Luzes locais: a luz escolhida pelo ReSTIR, integrada sobre sua esfera (ângulo sólido).
    if (r.light != NO_LIGHT && r.W > 0.0) {
      PointLight l = lights[r.light];
      vec3 L, E;
      float tSurface;
      if (sampleSphereLight(l, g.P, sample2D(sg, DIM_LIGHT), L, tSurface, E)) {
        evalBRDF(bc, L, -1.0, kd, ks);
#ifdef RAY_TRACING
        float vis = dot(sf.Nb, L) > 0.0 ? traceVisibility(shadowOrigin, L, tSurface) : 0.0;
#else
        float vis = 1.0;
#endif
        vec3 Li = E * preExposure() * r.W * vis * bumpShadowing(sf.Nb, g.N, L);
        diffuse += kd * Li;
        specular += ks * Li;
      }
    }

#ifdef RAY_TRACING
    // GI difusa: amostragem por cosseno => estimador = peso difuso × Li. Direções abaixo da faceta
    // geométrica (possíveis com normal map) atravessariam a superfície: são descartadas.
    vec3 dirD = cosineSampleHemisphere(g.N, sample2D(sg, DIM_DIFFUSE));
    if (dot(dirD, g.Ng) > 0.0) diffuse += bc.diffuseWeight * traceRadiance(origin, dirD, sg, 0u);

    // Especular: amostra VNDF => peso = F × G2/G1 (≈ G1(L)) × compensação de energia.
    float a = alphaFromRoughness(g.roughness);
    vec3 T, B;
    orthonormalBasis(g.N, T, B);
    vec3 Ht = sampleGGXVNDF(vec3(dot(g.V, T), dot(g.V, B), dot(g.V, g.N)), a, sample2D(sg, DIM_SPECULAR));
    vec3 H = dot(Ht, Ht) > 0.0 ? T * Ht.x + B * Ht.y + g.N * Ht.z : g.N;
    vec3 Lr = reflect(-g.V, H);
    float NoLr = dot(g.N, Lr);
    if (NoLr > 0.0 && dot(Lr, g.Ng) > 0.0)
      specular += traceRadiance(origin, Lr, sg, 1u) * F_Schlick(g.f0, g.f90, max(dot(g.V, H), 0.0)) *
                  G1_Smith(NoLr, a) * bc.energyCompensation;
#endif
  }
  diffuse /= float(spp);
  specular /= float(spp);

  uint debugView = uint(frame.environment.y);
  if (debugView != 0u) {
    float sunVis = sunVisSum / float(spp);
    vec3 v = debugView == 1u ? vec3(sunVis)
           : debugView == 2u ? vec3(max(dot(g.N, frame.sunDir.xyz), 0.0) * sunVis)
           : debugView == 3u ? g.Ng * 0.5 + 0.5
                             : vec3(max(dot(g.N, frame.sunDir.xyz), 0.0));
    imageStore(outDiffuse, id, vec4(v, 1.0));
    imageStore(outSpecular, id, vec4(0.0));
    return;
  }

#ifndef RAY_TRACING
  diffuse += bc.diffuseWeight * sampleSky(normalize(g.N + vec3(0, 1, 0))) * 0.8;
  vec3 R = reflect(-g.V, g.N);
  specular += specularAlbedo(g.f0, g.f90, bc.dfg) * bc.energyCompensation *
              sampleSky(normalize(mix(R, g.N, g.roughness * g.roughness)));
#endif

  // Rede de segurança: um único NaN se espalharia pelo denoiser e pelo histórico do TAA.
  if (any(isnan(diffuse)) || any(isinf(diffuse))) diffuse = vec3(0.0);
  if (any(isnan(specular)) || any(isinf(specular))) specular = vec3(0.0);
  imageStore(outDiffuse, id, vec4(clampHalf(diffuse), 1.0));
  imageStore(outSpecular, id, vec4(clampHalf(specular), 1.0));
}
