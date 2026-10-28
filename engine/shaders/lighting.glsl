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
#include "radiance_cache.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
// Escrito também: a visibilidade final volta para o reservatório (o próximo frame não reaproveita amostra oculta).
layout(set = 1, binding = 4, std430) buffer Reservoirs { Reservoir reservoirs[]; };
layout(set = 1, binding = 5, rgba16f) uniform writeonly image2D outDiffuse;
layout(set = 1, binding = 6, rgba16f) uniform writeonly image2D outSpecular;
// Entradas do NRD: z em espaço de câmera e normal + rugosidade na codificação R10G10B10A2 dele.
layout(set = 1, binding = 8, r32f) uniform writeonly image2D outViewZ;
layout(set = 1, binding = 9, rgb10_a2) uniform writeonly image2D outNrdNormalRoughness;

// Distância gravada para raios que não acertam nada (céu): "infinito" dentro do float16.
const float HIT_DIST_MISS = 60000.0;

// NRD_FrontEnd_PackNormalAndRoughness com NRD_NORMAL_ENCODING = R10G10B10A2_UNORM (octaédrico com sinal,
// John White 2017) e rugosidade linear — mesma conta do NRD.hlsli, em GLSL.
vec4 packNrdNormalRoughness(vec3 n, float roughness) {
  n /= abs(n.x) + abs(n.y) + abs(n.z);
  vec3 r;
  r.y = n.y * 0.5 + 0.5;
  r.x = n.x * 0.5 + r.y;
  r.y -= n.x * 0.5;
  roughness = max(roughness, 1.5 / 512.0);  // não pode ser 0: o sinal guarda o de n.z
  r.z = (n.z < 0.0 ? -roughness : roughness) * 0.5 + 0.5;
  return vec4(r, 0.0);  // material 0
}

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
// Luz direta num ponto atingido por um raio, para a direção -dir: emissão + sol (disco, com sombra) + uma luz local
// escolhida pela BVH de luzes (pdf exata, com sombra). BRDF completo (difuso + especular, compensação de energia).
vec3 hitDirectLighting(HitInfo hit, Surface s, BrdfContext bc, vec2 uSun, vec2 uLight, uint rngLight) {
  vec3 radiance = s.emissive * preExposure();
  vec3 kd, ks;
  // Ponto do raio: erro da ordem do epsilon do float × distância percorrida.
  vec3 shadowOrigin2 = offsetRayOrigin(hit.Pshadow, hit.Ng, RAY_BIAS_EXACT * hit.t);
  vec3 sunE = sunIrradianceAt(hit.P);
  if (dot(sunE, sunE) > 0.0) {
    vec3 Ls = sampleCone(frame.sunDir.xyz, cos(frame.params.x), uSun);
    if (dot(hit.N, Ls) > 0.0) {
      evalBRDF(bc, Ls, sqrt(min(s.roughness * s.roughness + 0.5 * frame.params.x, 1.0)), kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0) radiance += (kd * s.diffuseColor + ks) * sunE * traceVisibility(shadowOrigin2, Ls, 1e4);
    }
  }
  uint li;
  float pmf;
  if (sampleLightBvh(hit.P, hit.N, rngLight, li, pmf)) {
    vec3 L, E;
    float tSurface;
    if (sampleSphereLight(lights[li], hit.P, uLight, L, tSurface, E) && dot(hit.N, L) > 0.0) {
      evalBRDF(bc, L, -1.0, kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0 && dot(E, E) > 0.0)
        radiance += (kd * s.diffuseColor + ks) * E * (preExposure() / pmf) * traceVisibility(shadowOrigin2, L, tSurface * 0.99999);
    }
  }
  return radiance;
}

// REFERÊNCIA (--reference-pt): luz indireta que sai de `hit` para -dir, por path tracing de verdade — escolhe o lobo
// (difuso por cosseno ou especular por VNDF, na proporção das refletâncias), luz direta em cada vértice, céu ao
// escapar, até 8 rebotes com roleta russa. Ruído branco (PCG). Lento de propósito; mede o que o motor aproxima.
vec3 pathContinue(HitInfo hit, Surface s, BrdfContext bc, uint seed) {
  vec3 L = vec3(0.0), T = vec3(1.0);
  uint st = seed;
  for (int b = 0; b < 8; ++b) {
    vec3 specA = specularAlbedo(s.f0, s.f90, bc.dfg) * bc.energyCompensation;
    vec3 diffA = bc.diffuseWeight * s.diffuseColor;
    float ls = luminance(specA), ld = luminance(diffA);
    float pSpec = ld <= 0.0 ? 1.0 : clamp(ls / max(ls + ld, 1e-6), 0.05, 0.95);
    vec3 dir, w;
    if (rand(st) < pSpec) {
      float a = alphaFromRoughness(s.roughness);
      vec3 Tg, Bg;
      orthonormalBasis(bc.N, Tg, Bg);
      vec3 Ht = sampleGGXVNDF(vec3(dot(bc.V, Tg), dot(bc.V, Bg), dot(bc.V, bc.N)), a, rand2(st));
      vec3 H = dot(Ht, Ht) > 0.0 ? Tg * Ht.x + Bg * Ht.y + bc.N * Ht.z : bc.N;
      dir = reflect(-bc.V, H);
      float NoL = dot(bc.N, dir);
      if (NoL <= 0.0) break;
      w = F_Schlick(s.f0, s.f90, max(dot(bc.V, H), 0.0)) * G1_Smith(NoL, a) * bc.energyCompensation / pSpec;
    } else {
      dir = cosineSampleHemisphere(bc.N, rand2(st));
      w = diffA / (1.0 - pSpec);
    }
    if (dot(dir, hit.Ng) <= 0.0) break;
    T *= w;
    HitInfo next;
    if (!traceHit(offsetRayOrigin(hit.P, hit.Ng, RAY_BIAS_EXACT * hit.t), dir, 1e4, next)) {
      L += T * sampleSky(dir);
      break;
    }
    s = evalMaterial(materials[next.material], next.P, next.N, vec4(0.0), next.uv);
    bc = makeBrdfContext(next.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
    vec2 uSun = rand2(st), uLight = rand2(st);
    L += T * hitDirectLighting(next, s, bc, uSun, uLight, pcg(st));
    hit = next;
    if (b >= 2) {  // roleta russa
      float q = clamp(max(T.r, max(T.g, T.b)), 0.05, 1.0);
      if (rand(st) > q) break;
      T /= q;
    }
  }
  return L;
}

// Rugosidade abaixo da qual o ponto atingido por um raio reflete por um raio de verdade (o espelho mostra o reflexo do
// reflexo); acima, o especular indireto usa o cache (lobo largo ≈ irradiância).
const float GLOSSY_HIT_ROUGHNESS = 0.3;
// 1 em cada 4 pixels (rodando a cada frame) alimenta o cache de radiância.
bool cacheUpdatePixel() {
  uvec2 id = gl_GlobalInvocationID.xy;
  return ((id.x + 2u * id.y + frame.frameInfo.x) & 3u) == 0u;
}

// Luz indireta que um ponto atingido por raio manda de volta: M do cache (ou, sem dados ainda, o céu aproximado).
vec3 cachedIndirect(vec3 P, vec3 Ng, vec3 N) {
  vec3 M;
  return rcQuery(P, Ng, M) ? M : sampleSky(normalize(N + vec3(0, 1, 0))) * 0.5;
}

// Radiância que chega por um raio de 2º rebote: luz direta no ponto atingido + indireta pelo cache (difusa e,
// aproximada pelo mesmo M, especular). Ruído branco.
vec3 secondaryRadiance(vec3 origin, vec3 dir, inout uint st) {
  HitInfo h;
  if (!traceHit(origin, dir, 1e4, h)) return sampleSky(dir);
  Surface s = evalMaterial(materials[h.material], h.P, h.N, vec4(0.0), h.uv);
  BrdfContext c = makeBrdfContext(h.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
  vec2 uSun = rand2(st), uLight = rand2(st);
  vec3 L = hitDirectLighting(h, s, c, uSun, uLight, pcg(st));
  L += (s.diffuseColor * c.diffuseWeight + specularAlbedo(s.f0, s.f90, c.dfg) * c.energyCompensation) * cachedIndirect(h.P, h.Ng, h.N);
  return clampFirefly(L);
}

// Radiância que sai do ponto atingido em direção à origem do raio: luz direta + o que vem depois dela — no motor,
// indireta pelo cache de radiância (com um raio a mais em superfícies espelhadas); na referência, path tracing.
// `bounce` separa as dimensões de Sobol do raio difuso e do especular.
vec3 traceRadiance(vec3 origin, vec3 dir, SampleGen sg, uint bounce, out float hitDist) {
  HitInfo hit;
  hitDist = HIT_DIST_MISS;
  if (!traceHit(origin, dir, 1e4, hit)) return sampleSky(dir);
  hitDist = hit.t;
  Material m = materials[hit.material];
  Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv);
  BrdfContext bc = makeBrdfContext(hit.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
  uint dimBase = bounce * 3u;
  uint rngLight = pcg3d(uvec3(floatBitsToUint(sample2D(sg, DIM_HIT_LIGHT_PICK + dimBase).x), bounce, frame.frameInfo.x)).x;
  vec3 radiance = hitDirectLighting(hit, s, bc, sample2D(sg, DIM_HIT_SUN + dimBase), sample2D(sg, DIM_HIT_LIGHT + dimBase), rngLight);
  uint st = pcg3d(uvec3(gl_GlobalInvocationID.xy, frame.frameInfo.x * 8u + bounce)).x | 1u;
  if (pathReference()) return radiance + pathContinue(hit, s, bc, st);

  // Indireta difusa: cache de radiância (infinitos rebotes acumulados entre frames; auditoria 3.1).
  vec3 M;
  bool cached = rcQuery(hit.P, hit.Ng, M);
  if (!cached) M = sampleSky(normalize(hit.N + vec3(0, 1, 0))) * 0.5;  // célula nova: céu aproximado
  radiance += s.diffuseColor * bc.diffuseWeight * M;
  vec3 hitOrigin = offsetRayOrigin(hit.P, hit.Ng, RAY_BIAS_EXACT * hit.t);
  // Indireta especular: superfícies espelhadas refletem por um raio (amostra VNDF); as outras, pelo cache.
  if (s.roughness < GLOSSY_HIT_ROUGHNESS) {
    float a = alphaFromRoughness(s.roughness);
    vec3 T, B;
    orthonormalBasis(bc.N, T, B);
    vec3 Ht = sampleGGXVNDF(vec3(dot(bc.V, T), dot(bc.V, B), dot(bc.V, bc.N)), a, rand2(st));
    vec3 H = dot(Ht, Ht) > 0.0 ? T * Ht.x + B * Ht.y + bc.N * Ht.z : bc.N;
    vec3 Lr = reflect(-bc.V, H);
    float NoL = dot(bc.N, Lr);
    if (NoL > 0.0 && dot(Lr, hit.Ng) > 0.0)
      radiance += secondaryRadiance(hitOrigin, Lr, st) * F_Schlick(s.f0, s.f90, max(dot(bc.V, H), 0.0)) *
                  G1_Smith(NoL, a) * bc.energyCompensation;
  } else {
    radiance += specularAlbedo(s.f0, s.f90, bc.dfg) * bc.energyCompensation *
                (cached ? M : sampleSky(reflect(dir, hit.N)));
  }
  // Alimenta o cache no ponto atingido (fora da tela também): um raio difuso a partir dele.
  if (bounce == 0u && cacheUpdatePixel()) {
    vec3 d2 = cosineSampleHemisphere(hit.N, rand2(st));
    if (dot(d2, hit.Ng) > 0.0) rcAdd(hit.P, hit.Ng, secondaryRadiance(hitOrigin, d2, st));
  }
  return clampFirefly(radiance);
}

// Referência por força bruta para medir o ReSTIR: todas as luzes no alcance, 16 amostras estratificadas
// na esfera de cada uma, com sombra. Lento de propósito (só --debug-view luzes-ref).
vec3 localLightsReference(PixelSurface sf, BrdfContext bc) {
  GSample g = sf.g;
  vec3 origin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
  vec3 sum = vec3(0.0), kd, ks;
  for (uint li = 0u; li < frame.frameInfo.z; ++li) {
    PointLight l = lights[li];
    if (distance(l.position, g.P) >= l.range) continue;
    for (uint s = 0u; s < 16u; ++s) {
      vec2 u = (vec2(s % 4u, s / 4u) + vec2(0.5)) / 4.0;
      vec3 L, E;
      float tSurface;
      if (!sampleSphereLight(l, g.P, u, L, tSurface, E) || dot(sf.Nb, L) <= 0.0) continue;
      evalBRDF(bc, L, -1.0, kd, ks);
      if (dot(kd + ks, vec3(1.0)) <= 0.0) continue;
      sum += (kd * g.diffuseColor + ks) * E * bumpShadowing(sf.Nb, g.N, L) * traceVisibility(origin, L, tSurface * 0.99999) / 16.0;
    }
  }
  return sum * preExposure();
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
    imageStore(outViewZ, id, vec4(1e9));  // além do alcance do NRD: céu
    imageStore(outNrdNormalRoughness, id, vec4(0.5, 0.5, 1.0, 0.0));
    return;
  }
  imageStore(outViewZ, id, vec4((frame.view * vec4(g.P, 1.0)).z));
  imageStore(outNrdNormalRoughness, id, packNrdNormalRoughness(g.N, g.roughness));
  vec3 diffuse = vec3(0.0), specular = vec3(0.0), kd, ks;
#ifdef RAY_TRACING
  // Raios de GI/reflexo saem do ponto exato; raios de sombra, do ponto corrigido para o terminator.
  vec3 origin = offsetRayOrigin(g.P, g.Ng, sf.rayBias);
  vec3 shadowOrigin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
#endif
  uint resIdx = uint(id.y) * uint(frame.renderSize.x) + uint(id.x);
  Reservoir r = reservoirs[resIdx];
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
  // Distância dos raios indiretos para o NRD: média no difuso, menor acerto no especular (NRD_FrontEnd_SpecHitDistAveraging).
  float diffHitSum = 0.0, diffHitCount = 0.0, specHitMin = HIT_DIST_MISS + 1.0;
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

#ifdef RAY_TRACING
    // GI difusa: amostragem por cosseno => estimador = peso difuso × Li. Direções abaixo da faceta
    // geométrica (possíveis com normal map) atravessariam a superfície: são descartadas.
    vec3 dirD = cosineSampleHemisphere(g.N, sample2D(sg, DIM_DIFFUSE));
    if (dot(dirD, g.Ng) > 0.0) {
      float t;
      vec3 Li = traceRadiance(origin, dirD, sg, 0u, t);
      diffuse += bc.diffuseWeight * Li;
      if (!pathReference() && cacheUpdatePixel()) rcAdd(g.P, g.Ng, Li);  // M da superfície visível
      diffHitSum += t;
      diffHitCount += 1.0;
    }

    // Especular: amostra VNDF => peso = F × G2/G1 (≈ G1(L)) × compensação de energia.
    float a = alphaFromRoughness(g.roughness);
    vec3 T, B;
    orthonormalBasis(g.N, T, B);
    vec3 Ht = sampleGGXVNDF(vec3(dot(g.V, T), dot(g.V, B), dot(g.V, g.N)), a, sample2D(sg, DIM_SPECULAR));
    vec3 H = dot(Ht, Ht) > 0.0 ? T * Ht.x + B * Ht.y + g.N * Ht.z : g.N;
    vec3 Lr = reflect(-g.V, H);
    float NoLr = dot(g.N, Lr);
    if (NoLr > 0.0 && dot(Lr, g.Ng) > 0.0) {
      float t;
      specular += traceRadiance(origin, Lr, sg, 1u, t) * F_Schlick(g.f0, g.f90, max(dot(g.V, H), 0.0)) *
                  G1_Smith(NoLr, a) * bc.energyCompensation;
      specHitMin = min(specHitMin, t);
    }
#endif
  }
  diffuse /= float(spp);
  specular /= float(spp);

  // Luzes locais: o ponto de luz escolhido pelo ReSTIR (o mesmo testado e reaproveitado), com sombra.
  vec3 localLight = vec3(0.0);
  if (r.light != NO_LIGHT && r.W > 0.0) {
    vec3 kdl, ksl, L;
    float dist;
    if (lightContribution(g, bc, r.light, r.uv, kdl, ksl, L, dist) && dot(sf.Nb, L) > 0.0) {
#ifdef RAY_TRACING
      float vis = traceVisibility(shadowOrigin, L, dist * 0.99999);
      if (vis == 0.0) reservoirs[resIdx].W = 0.0;  // visibilidade final reaproveitada no próximo frame
#else
      float vis = 1.0;
#endif
      float w = r.W * vis * bumpShadowing(sf.Nb, g.N, L) * preExposure();
      diffuse += kdl * w;
      specular += ksl * w;
      localLight = (kdl * g.diffuseColor + ksl) * w;
    }
  }

  uint debugView = uint(frame.environment.y);
  if (debugView == 5u || debugView == 6u) {
    vec3 v = localLight;
#ifdef RAY_TRACING
    if (debugView == 6u) v = localLightsReference(sf, bc);
#endif
    v = v / preExposure() * frame.environment.z;
    imageStore(outDiffuse, id, vec4(v / (1.0 + v), 1.0));  // codificação inversível: nada satura no PNG
    imageStore(outSpecular, id, vec4(0.0));
    return;
  }
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
  specular /= specularDemodulation(bc);  // remultiplicado em composite.comp
  float diffHit = diffHitCount > 0.0 ? diffHitSum / diffHitCount : 0.0;  // 0 = nenhum raio lançado
  float specHit = specHitMin <= HIT_DIST_MISS ? specHitMin : 0.0;
  if (reblurSignal()) {
    // REBLUR_FrontEnd_PackRadianceAndNormHitDist: YCoCg + distância normalizada por
    // _REBLUR_GetHitDistanceNormalization com os parâmetros padrão (A = 3, B = 0,1, C = 20).
    float viewZ = abs((frame.view * vec4(g.P, 1.0)).z);
    float smc = (1.0 - exp2(-200.0 * g.roughness * g.roughness)) * sqrt(clamp(g.roughness, 0.0, 1.0));
    float specNorm = (3.0 + viewZ * 0.1) * mix(20.0, 1.0, smc);
    float diffNorm = (3.0 + viewZ * 0.1);  // rugosidade 1: curva = 1
    imageStore(outDiffuse, id, vec4(nrdLinearToYCoCg(clampHalf(diffuse)), clamp(diffHit / diffNorm, 0.0, 1.0)));
    imageStore(outSpecular, id, vec4(nrdLinearToYCoCg(clampHalf(specular)), clamp(specHit / specNorm, 0.0, 1.0)));
  } else {
    imageStore(outDiffuse, id, vec4(clampHalf(diffuse), diffHit));
    imageStore(outSpecular, id, vec4(clampHalf(specular), specHit));
  }
}
