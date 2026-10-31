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
#define GLASS_SHADOWS 1  // traceTransmittance tinge pela cor do vidro (glassTransmittanceAlong, abaixo)
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

// Cabelo: modelo de Marschner (2003) com as aproximações de Karis ("Physically Based Hair Shading in Unreal", 2016) —
// três caminhos da luz no fio: R (reflete na cutícula, branco, deslocado para a raiz), TT (atravessa o fio: brilho
// colorido contra a luz) e TRT (entra, reflete dentro e sai: o segundo reflexo colorido). Termo longitudinal por
// gaussiana em θ, azimutal por aproximações em φ, Fresnel da cutícula (IOR 1,55) e absorção pela cor do cabelo.
// Difuso (espalhamento múltiplo entre fios) por um termo envolvente. kd sem o albedo (demodulado), ks já colorido.
float hairG(float B, float theta) { return exp(-0.5 * theta * theta / (B * B)) / (2.5066283 * B); }
float hairF(float cosTheta) { return 0.0465 + 0.9535 * pow(1.0 - cosTheta, 5.0); }
void hairBSDF(vec3 T, vec3 N, vec3 V, vec3 L, vec3 color, float roughness, out vec3 kd, out vec3 ks) {
  float VoL = dot(V, L);
  float sinL = clamp(dot(T, L), -1.0, 1.0), sinV = clamp(dot(T, V), -1.0, 1.0);
  float cosD = cos(0.5 * abs(asin(sinV) - asin(sinL)));
  vec3 Lp = L - sinL * T, Vp = V - sinV * T;
  float cosPhi = dot(Lp, Vp) * inversesqrt(dot(Lp, Lp) * dot(Vp, Vp) + 1e-4);
  float cosHalfPhi = sqrt(clamp(0.5 + 0.5 * cosPhi, 0.0, 1.0));
  float nPrime = 1.19 / cosD + 0.36 * cosD;
  const float SHIFT = 0.035;
  float r2 = roughness * roughness;
  float B0 = max(r2, 0.02), B1 = max(r2 * 0.5, 0.01), B2 = max(r2 * 2.0, 0.04);
  vec3 S = vec3(0.0);
  // R
  S += vec3(hairG(B0 * 1.41421 * cosD, sinL + sinV + 2.0 * SHIFT) * 0.25 * cosHalfPhi * hairF(sqrt(clamp(0.5 + 0.5 * VoL, 0.0, 1.0))));
  // TT
  float a = 1.0 / nPrime;
  float h = cosHalfPhi * (1.0 + a * (0.6 - 0.8 * cosPhi));
  float f = hairF(cosD * sqrt(clamp(1.0 - h * h, 0.0, 1.0)));
  vec3 Tp = pow(max(color, vec3(1e-4)), vec3(0.5 * sqrt(max(1.0 - h * h * a * a, 0.0)) / cosD));
  S += hairG(B1, sinL + sinV - SHIFT) * exp(-3.65 * cosPhi - 3.98) * (1.0 - f) * (1.0 - f) * Tp;
  // TRT
  f = hairF(cosD * 0.5);
  Tp = pow(max(color, vec3(1e-4)), vec3(0.8 / cosD));
  S += hairG(B2, sinL + sinV - 4.0 * SHIFT) * exp(17.0 * cosPhi - 16.78) * (1.0 - f) * (1.0 - f) * f * Tp;
  float cosL = sqrt(max(1.0 - sinL * sinL, 0.0));
  ks = S * cosL;  // irradiância no fio: cos em relação à seção (Karis)
  kd = vec3(mix(0.25, 1.0, clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0)) * cosL / PI);
}

// Tangente do fio de cabelo no pixel (triângulo do visibility buffer, média das tangentes dos 3 vértices).
vec3 hairTangentAt(ivec2 id) {
  uvec2 vis = texelFetch(gVisibility, id, 0).xy;
  if (vis.x == 0u) return vec3(0.0, 1.0, 0.0);
  uint obj = vis.x - 1u;
  InstanceData d = instances[obj];
  IndexData ib = IndexData(d.indices);
  VertexData vb = VertexData(d.vertices);
  vec3 t = vec3(0.0);
  for (uint k = 0u; k < 3u; ++k) {
    uint i = ib.i[vis.y * 3u + k];
    t += vec3(vb.v[i * 12u + 8u], vb.v[i * 12u + 9u], vb.v[i * 12u + 10u]);
  }
  return normalize(mat3(objects[obj].model) * t);
}

// Dimensões da sequência de Sobol por pixel (cada uma é um par 2D independente).
const uint DIM_SUN = 0u, DIM_LIGHT = 1u, DIM_DIFFUSE = 2u, DIM_SPECULAR = 3u;
const uint DIM_HIT_SUN = 4u, DIM_HIT_LIGHT = 5u, DIM_HIT_LIGHT_PICK = 6u;  // + 3 × rebote (0 GI, 1 reflexo, 2 vidro)
const uint DIM_TRANSMISSION = 16u, DIM_CLEARCOAT = 17u;  // (13–15: rebote 3, o raio do verniz)

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
//   countSampledEmission: soma também a emissão de luzes amostradas — só para raios cujo lobo a amostragem de luzes
//   não cobre (a transmissão do vidro: o ReSTIR avalia só a reflexão).
vec3 hitDirectLighting(HitInfo hit, Surface s, BrdfContext bc, vec2 uSun, vec2 uLight, uint rngLight,
                       bool countSampledEmission) {
  // Emissão já amostrada como luz (luzes de malha, globo de lâmpada) entra pela luz direta abaixo; somá-la aqui também
  // dobraria a energia (auditoria 1.4).
  bool emissionSampled = !countSampledEmission && (materials[hit.material].flags & MATERIAL_EMISSION_SAMPLED) != 0u;
  vec3 radiance = emissionSampled ? vec3(0.0) : s.emissive * preExposure();
  vec3 kd, ks;
  // Ponto do raio: erro da ordem do epsilon do float × distância percorrida.
  vec3 shadowOrigin2 = offsetRayOrigin(hit.Pshadow, hit.Ng, RAY_BIAS_EXACT * hit.t);
  vec3 sunE = sunIrradianceAt(hit.P);
  if (dot(sunE, sunE) > 0.0) {
    vec3 Ls = sampleCone(frame.sunDir.xyz, cos(frame.params.x), uSun);
    if (dot(hit.N, Ls) > 0.0) {
      evalBRDF(bc, Ls, sqrt(min(s.roughness * s.roughness + 0.5 * frame.params.x, 1.0)), kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0) radiance += (kd * s.diffuseColor + ks) * sunE * traceTransmittance(shadowOrigin2, Ls, 1e4);
    }
  }
  uint li;
  float pmf;
  if (sampleLightBvh(hit.P, hit.N, rngLight, li, pmf)) {
    vec3 L, E;
    float tSurface;
    if (sampleLightForShading(li, hit.P, uLight, L, tSurface, E) && dot(hit.N, L) > 0.0) {
      evalBRDF(bc, L, -1.0, kd, ks);
      if (dot(kd + ks, vec3(1.0)) > 0.0 && dot(E, E) > 0.0)
        radiance += (kd * s.diffuseColor + ks) * E * (preExposure() / pmf) * traceVisibility(shadowOrigin2, L, tSurface * 0.99999);
    }
  }
  return radiance;
}

// Raio que atravessa vidro de parede fina (KHR_materials_transmission): passa reto por até 4 superfícies
// transmissivas, multiplicando T pelo que cada uma deixa passar (transmissão × cor × (1 − Fresnel)); a reflexão e o
// difuso do próprio vidro nesses raios secundários ficam de fora (poucos %). Devolve falso no céu; `blocked` = o vidro
// absorveu tudo ou havia camadas demais.
// Vidro com volume (KHR_materials_volume): a direção dobra pela lei de Snell ao entrar e ao sair, e dentro do
// material a luz é absorvida pela lei de Beer (attenuationColor a cada attenuationDistance metros). Reflexão interna
// total devolve o raio para dentro. `inside` = o raio está dentro de um volume.
vec3 refractThroughSurface(vec3 dir, vec3 Ng, float ior, bool inside, out bool reflected) {
  vec3 n = dot(dir, Ng) < 0.0 ? Ng : -Ng;  // contra o raio
  float eta = inside ? ior : 1.0 / ior;
  vec3 r = refract(dir, n, eta);
  reflected = dot(r, r) == 0.0;
  return reflected ? reflect(dir, n) : normalize(r);
}

// Transmitância das lâminas de vidro entre origin e origin + dir × tMax (só instâncias de vidro: RAY_MASK_GLASS).
vec3 glassTransmittanceAlong(vec3 origin, vec3 dir, float tMax) {
  vec3 T = vec3(1.0);
  bool inside = false;
  vec3 absorb = vec3(0.0);
  for (int layer = 0; layer < 6 && tMax > 0.0; ++layer) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas, gl_RayFlagsNoneEXT, RAY_MASK_GLASS, origin, 0.0, dir, tMax);
    RESOLVE_CANDIDATES(rq)
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) break;
    float t = rayQueryGetIntersectionTEXT(rq, true);
    if (inside) T *= exp(-absorb * t);
    uint inst = uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true));
    Material m = materials[instances[inst].material];
    T *= m.transmission * (inside ? vec3(1.0) : m.baseColor.rgb) * 0.96;  // ~4% refletido em cada face
    if (m.thickness > 0.0) {  // volume: a sombra não desvia (aproximação), mas absorve dentro
      inside = !inside;
      absorb = (inside && m.attenuationDistance > 0.0) ? -log(max(m.attenuationColor, vec3(1e-4))) / m.attenuationDistance : vec3(0.0);
    }
    if (max(T.r, max(T.g, T.b)) < 1e-3) return vec3(0.0);
    origin += dir * (t + 1e-4 * (1.0 + t));
    tMax -= t + 1e-4 * (1.0 + t);
  }
  return T;
}

// Raio que atravessa vidro (KHR_materials_transmission): em parede fina passa reto; com volume refrata na entrada e
// na saída e absorve no caminho. Até 6 superfícies transmissivas, multiplicando T pelo que cada uma deixa passar
// (transmissão × cor × (1 − Fresnel)); a reflexão e o difuso do próprio vidro nesses raios secundários ficam de fora
// (poucos %). Devolve falso no céu (dir_out = direção final); `blocked` = absorveu tudo ou camadas demais.
bool traceHitThroughGlassDir(vec3 origin, inout vec3 dir, float tMax, out HitInfo hit, out vec3 T, out bool blocked,
                             bool inside, vec3 absorb) {  // absorb: coeficiente de absorção do volume de partida (1/m)
  T = vec3(1.0);
  blocked = false;
  for (int layer = 0; layer < 6; ++layer) {
    if (!traceHit(origin, dir, tMax, hit)) return false;
    if (inside) T *= exp(-absorb * hit.t);  // Beer no trecho dentro do volume
    Material m = materials[hit.material];
    if ((m.flags & MATERIAL_TRANSMISSIVE) == 0u) return true;
    Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv, hit.color);
    T *= s.transmission * (inside ? vec3(1.0) : s.transmissionTint) * (vec3(1.0) - F_Schlick(s.f0, s.f90, abs(dot(hit.N, dir))));
    if (max(T.r, max(T.g, T.b)) < 1e-4) break;
    if (m.thickness > 0.0) {  // volume: refrata e entra/sai
      bool reflected;
      dir = refractThroughSurface(dir, hit.Ng, m.ior, inside, reflected);
      if (!reflected) inside = !inside;
      absorb = (inside && m.attenuationDistance > 0.0) ? -log(max(m.attenuationColor, vec3(1e-4))) / m.attenuationDistance : vec3(0.0);
      origin = offsetRayOrigin(hit.P, dot(dir, hit.Ng) > 0.0 ? hit.Ng : -hit.Ng, RAY_BIAS_EXACT * hit.t);
    } else {
      origin = offsetRayOrigin(hit.P, -hit.Ng, RAY_BIAS_EXACT * hit.t);  // parede fina: continua do outro lado
    }
    tMax -= hit.t;
  }
  blocked = true;
  return true;
}
bool traceHitThroughGlass(vec3 origin, vec3 dir, float tMax, out HitInfo hit, out vec3 T, out bool blocked) {
  return traceHitThroughGlassDir(origin, dir, tMax, hit, T, blocked, false, vec3(0.0));
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
      w = F_Schlick(s.f0, s.f90, max(dot(bc.V, H), 0.0)) * vndfWeight(bc.NoV, NoL, a) * bc.energyCompensation / pSpec;
    } else {
      dir = cosineSampleHemisphere(bc.N, rand2(st));
      w = diffA / (1.0 - pSpec);
    }
    if (dot(dir, hit.Ng) <= 0.0) break;
    T *= w;
    HitInfo next;
    vec3 Tg;
    bool blocked;
    if (!traceHitThroughGlass(offsetRayOrigin(hit.P, hit.Ng, RAY_BIAS_EXACT * hit.t), dir, 1e4, next, Tg, blocked)) {
      L += T * Tg * sampleSky(dir);
      break;
    }
    if (blocked) break;
    T *= Tg;
    s = evalMaterial(materials[next.material], next.P, next.N, vec4(0.0), next.uv, next.color);
    bc = makeBrdfContext(next.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
    vec2 uSun = rand2(st), uLight = rand2(st);
    L += T * hitDirectLighting(next, s, bc, uSun, uLight, pcg(st), false);
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
  vec3 Tg;
  bool blocked;
  if (!traceHitThroughGlass(origin, dir, 1e4, h, Tg, blocked)) return clampFirefly(Tg * sampleSky(dir));
  if (blocked) return vec3(0.0);
  Surface s = evalMaterial(materials[h.material], h.P, h.N, vec4(0.0), h.uv, h.color);
  BrdfContext c = makeBrdfContext(h.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
  vec2 uSun = rand2(st), uLight = rand2(st);
  vec3 L = hitDirectLighting(h, s, c, uSun, uLight, pcg(st), false);
  L += (s.diffuseColor * c.diffuseWeight + specularAlbedo(s.f0, s.f90, c.dfg) * c.energyCompensation) * cachedIndirect(h.P, h.Ng, h.N);
  return clampFirefly(Tg * L);
}

// Radiância que sai de `hit` para a origem do raio (ver traceRadiance, logo abaixo).
vec3 traceRadianceAt(HitInfo hit, vec3 dir, SampleGen sg, uint bounce, bool countSampledEmission) {
  Material m = materials[hit.material];
  Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv, hit.color);
  BrdfContext bc = makeBrdfContext(hit.N, -dir, s.f0, s.f90, s.roughness, energyCompensationEnabled());
  uint dimBase = bounce * 3u;
  uint rngLight = pcg3d(uvec3(floatBitsToUint(sample2D(sg, DIM_HIT_LIGHT_PICK + dimBase).x), bounce, frame.frameInfo.x)).x;
  vec3 radiance = hitDirectLighting(hit, s, bc, sample2D(sg, DIM_HIT_SUN + dimBase), sample2D(sg, DIM_HIT_LIGHT + dimBase), rngLight,
                                    countSampledEmission);
  uint st = pcg3d(uvec3(gl_GlobalInvocationID.xy, frame.frameInfo.x * 8u + bounce)).x | 1u;
  // Mesmo limite de vagalumes do motor por amostra: a referência mede reflexos e luz indireta, não cáusticas (sol
  // refletido por espelho, que motor de jogo nenhum calcula e o limite corta — medido: era o resto do viés).
  if (pathReference()) return clampFirefly(radiance + pathContinue(hit, s, bc, st));

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
                  vndfWeight(bc.NoV, NoL, a) * bc.energyCompensation;
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

// Radiância que sai do ponto atingido em direção à origem do raio: luz direta + o que vem depois dela — no motor,
// indireta pelo cache de radiância (com um raio a mais em superfícies espelhadas); na referência, path tracing.
// `bounce` separa as dimensões de Sobol do raio difuso e do especular.
//   countSampledEmission: ver hitDirectLighting (verdadeiro só para o raio de transmissão do vidro).
vec3 traceRadianceFrom(vec3 origin, vec3 dir, SampleGen sg, uint bounce, out float hitDist, bool countSampledEmission,
                       bool inside, vec3 absorb) {
  HitInfo hit;
  hitDist = HIT_DIST_MISS;
  vec3 Tg;
  bool blocked;
  if (!traceHitThroughGlassDir(origin, dir, 1e4, hit, Tg, blocked, inside, absorb)) return Tg * sampleSky(dir);
  if (blocked) return vec3(0.0);
  hitDist = hit.t;
  return Tg * traceRadianceAt(hit, dir, sg, bounce, countSampledEmission);
}
vec3 traceRadiance(vec3 origin, vec3 dir, SampleGen sg, uint bounce, out float hitDist, bool countSampledEmission) {
  return traceRadianceFrom(origin, dir, sg, bounce, hitDist, countSampledEmission, false, vec3(0.0));
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
      if (!sampleLightForShading(li, g.P, u, L, tSurface, E) || dot(sf.Nb, L) <= 0.0) continue;
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
  // Material do pixel relido só onde o G-buffer não basta: vidro (transmissão) e verniz (duas camadas).
  Surface px;
  px.transmission = 0.0;
  px.clearcoat = 0.0;
  uint pxFlags = sf.material != NO_MATERIAL ? materials[sf.material].flags : 0u;
  if ((pxFlags & (MATERIAL_TRANSMISSIVE | MATERIAL_CLEARCOAT)) != 0u)  // (SSS só lê o raio do material)
    px = evalMaterial(materials[sf.material], g.P, g.N, vec4(0.0), sf.uv, sf.color);
  // A demodulação do especular usa o contexto do G-buffer: a composição remultiplica pelo mesmo valor.
  BrdfContext bcDemod = brdfContextFor(g);
  // Verniz (KHR_materials_clearcoat): a base usa a própria rugosidade; o verniz é um GGX dielétrico (f0 = 4%) na
  // normal suave, e a base recebe só o que o verniz deixa passar (1 − cc × F).
  float coat = px.clearcoat;
  BrdfContext bcCoat;
  float coatAtten = 1.0;
  vec3 coatSpec = vec3(0.0);
  if (coat > 0.0) {
    if (coat >= 0.5) g.roughness = px.roughness;  // o G-buffer guardou a do verniz
    bcCoat = makeBrdfContext(sf.Nb, g.V, vec3(0.04), 1.0, px.clearcoatRoughness, energyCompensationEnabled());
    coatAtten = 1.0 - coat * F_Schlick(vec3(0.04), 1.0, bcCoat.NoV).x;
  }
  // Oclusão do material (glTF): detalhe menor que a geometria, aplicado só à luz indireta — difusa direto, especular
  // pela aproximação de Lagarde (2014) que a converte para o lobo especular.
  float aoDiffuse = g.occlusion;
  float aoSpecular = clamp(pow(max(dot(g.N, g.V), 0.0) + g.occlusion, exp2(-16.0 * g.roughness - 1.0)) - 1.0 + g.occlusion, 0.0, 1.0);
#ifdef RAY_TRACING
  // Raios de GI/reflexo saem do ponto exato; raios de sombra, do ponto corrigido para o terminator.
  vec3 origin = offsetRayOrigin(g.P, g.Ng, sf.rayBias);
  vec3 shadowOrigin = offsetRayOrigin(sf.Pshadow, g.Ng, sf.rayBias);
#endif
  uint resIdx = uint(id.y) * uint(frame.renderSize.x) + uint(id.x);
  Reservoir r = reservoirs[resIdx];
  vec3 sunE = sunIrradianceAt(g.P);
  BrdfContext bc = brdfContextFor(g);
  // Cabelo: troca o BRDF do sol pelo do fio (a tangente vem do triângulo exato).
  bool hair = (pxFlags & MATERIAL_HAIR) != 0u;
  vec3 hairT = hair ? hairTangentAt(id) : vec3(0.0);
  vec3 hairColor = hair ? materials[sf.material].baseColor.rgb : vec3(0.0);
  float hairRough = hair ? materials[sf.material].roughness : 0.0;
  vec3 coatSunKs = vec3(0.0);
  if (coat > 0.0 && dot(sunE, sunE) > 0.0) {
    vec3 unused;
    evalBRDF(bcCoat, frame.sunDir.xyz, sqrt(min(px.clearcoatRoughness * px.clearcoatRoughness + 0.5 * frame.params.x, 1.0)),
             unused, coatSunKs);
  }

  // Sol como disco de verdade: o BRDF usa a direção central com a rugosidade alargada pelo tamanho
  // angular do disco (Karis 2013) — sem o pico de um sol pontual. É determinístico: fica fora do laço.
  // O termo de relevo (Chiang 2019) suaviza o terminator do normal map.
  vec3 sunKd = vec3(0.0), sunKs = vec3(0.0);
  if (dot(sunE, sunE) > 0.0) {
    evalBRDF(bc, frame.sunDir.xyz, sqrt(min(g.roughness * g.roughness + 0.5 * frame.params.x, 1.0)), sunKd, sunKs);
    float bump = bumpShadowing(sf.Nb, g.N, frame.sunDir.xyz);
    sunKd *= bump;
    sunKs *= bump;
    if (hair) hairBSDF(hairT, g.N, g.V, frame.sunDir.xyz, hairColor, hairRough, sunKd, sunKs);
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
      vec3 vis = frame.shadowParams.y > 0.5 ? vec3(sunShadowMap(g.P, g.Ng))  // --shadow-maps: comparação/desempenho
                : (hair || dot(sf.Nb, L) > 0.0) ? traceTransmittance(shadowOrigin, L, 1e4) : vec3(0.0);
#else
      vec3 vis = vec3(shadowMapsEnabled() ? sunShadowMap(g.P, g.Ng) : 1.0);  // sem ray tracing: shadow maps em cascata
#endif
      sunVisSum += luminance(vis);
      diffuse += sunKd * sunE * vis;
      specular += sunKs * sunE * vis;
      coatSpec += coatSunKs * sunE * vis;
#ifdef RAY_TRACING
      // Transmissão em partes finas de material com espalhamento subsuperficial (orelha, nariz, folha contra a luz;
      // Jimenez 2010): um raio para dentro mede a espessura até a outra face, um raio de sombra de lá para o sol, e a
      // luz atravessa atenuada por canal (o vermelho, que anda mais na pele, passa mais).
      if ((pxFlags & MATERIAL_SUBSURFACE) != 0u && dot(sf.Nb, frame.sunDir.xyz) < 0.2) {
        HitInfo back;
        vec3 inward = -sf.Nb;
        if (traceHit(offsetRayOrigin(g.P, -g.Ng, sf.rayBias), inward, 0.05, back)) {
          vec3 radius = max(materials[sf.material].subsurfaceRadius, vec3(1e-5));
          vec3 Tr = exp(-back.t / (radius * 3.0));
          float lightSide = clamp(-dot(sf.Nb, frame.sunDir.xyz) * 0.5 + 0.5, 0.0, 1.0);  // envolve o terminador
          if (max(Tr.r, max(Tr.g, Tr.b)) > 1e-3) {
            // A normal do acerto aponta contra o raio (para dentro): sai para o outro lado, fora do material.
            vec3 exitP = offsetRayOrigin(back.P, -back.Ng, RAY_BIAS_EXACT * back.t);
            diffuse += Tr * sunE * lightSide * (1.0 / PI) * traceTransmittance(exitP, frame.sunDir.xyz, 1e4);
          }
        }
      }
#endif
    }

#ifdef RAY_TRACING
    // GI difusa: amostragem por cosseno => estimador = peso difuso × Li. Direções abaixo da faceta
    // geométrica (possíveis com normal map) atravessariam a superfície: são descartadas.
    vec3 dirD = cosineSampleHemisphere(g.N, sample2D(sg, DIM_DIFFUSE));
    if (dot(dirD, g.Ng) > 0.0) {
      float t;
      vec3 Li = traceRadiance(origin, dirD, sg, 0u, t, false);
      diffuse += bc.diffuseWeight * Li * aoDiffuse;
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
      specular += traceRadiance(origin, Lr, sg, 1u, t, false) * F_Schlick(g.f0, g.f90, max(dot(g.V, H), 0.0)) *
                  vndfWeight(bc.NoV, NoLr, a) * bc.energyCompensation * aoSpecular;
      specHitMin = min(specHitMin, t);
    }

    // Verniz: um raio de reflexo próprio (VNDF com a rugosidade do verniz, na normal suave).
    if (coat > 0.0) {
      float ac = alphaFromRoughness(px.clearcoatRoughness);
      vec3 Tc, Bc;
      orthonormalBasis(sf.Nb, Tc, Bc);
      vec3 Hct = sampleGGXVNDF(vec3(dot(g.V, Tc), dot(g.V, Bc), dot(g.V, sf.Nb)), ac, sample2D(sg, DIM_CLEARCOAT));
      vec3 Hc = dot(Hct, Hct) > 0.0 ? Tc * Hct.x + Bc * Hct.y + sf.Nb * Hct.z : sf.Nb;
      vec3 Lc = reflect(-g.V, Hc);
      float NoLc = dot(sf.Nb, Lc);
      if (NoLc > 0.0 && dot(Lc, g.Ng) > 0.0) {
        float t;
        coatSpec += traceRadiance(origin, Lc, sg, 3u, t, false) * F_Schlick(vec3(0.04), 1.0, max(dot(g.V, Hc), 0.0)) *
                    vndfWeight(bcCoat.NoV, NoLc, ac) * bcCoat.energyCompensation * aoSpecular;
        specHitMin = min(specHitMin, t);
      }
    }

    // Vidro de parede fina (KHR_materials_transmission): o lobo especular espelhado para o outro lado da superfície
    // (a luz atravessa sem desviar, com o mesmo espalhamento da rugosidade), peso (1 − F) × cor × transmissão. Vai no
    // sinal especular (nítido, depende da vista). O ReSTIR só avalia a reflexão, então este raio conta a emissão das
    // luzes amostradas (a lâmpada dentro da lanterna aparece pelo vidro).
    if (px.transmission > 0.0) {
      vec3 Ht2 = sampleGGXVNDF(vec3(dot(g.V, T), dot(g.V, B), dot(g.V, g.N)), a, sample2D(sg, DIM_TRANSMISSION));
      vec3 H2 = dot(Ht2, Ht2) > 0.0 ? T * Ht2.x + B * Ht2.y + g.N * Ht2.z : g.N;
      vec3 Lm = reflect(-g.V, H2);
      float NoLm = dot(g.N, Lm);
      vec3 Lt = Lm - 2.0 * dot(Lm, g.Ng) * g.Ng;  // espelhado pelo plano do triângulo: segue para trás do vidro
      Material pm = materials[sf.material];
      bool volume = pm.thickness > 0.0;
      if (volume) {  // com volume: refrata pela microfaceta sorteada e começa dentro do material
        vec3 r = refract(-g.V, H2, 1.0 / pm.ior);
        Lt = dot(r, r) > 0.0 ? normalize(r) : Lt;
      }
      if (NoLm > 0.0 && dot(Lt, g.Ng) < 0.0) {
        float t;
        vec3 w = (vec3(1.0) - F_Schlick(g.f0, g.f90, max(dot(g.V, H2), 0.0))) * px.transmissionTint *
                 px.transmission * vndfWeight(bc.NoV, NoLm, a);
        vec3 absorb = (volume && pm.attenuationDistance > 0.0)
                          ? -log(max(pm.attenuationColor, vec3(1e-4))) / pm.attenuationDistance : vec3(0.0);
        specular += traceRadianceFrom(offsetRayOrigin(g.P, -g.Ng, sf.rayBias), Lt, sg, 2u, t, true, volume, absorb) * w;
        specHitMin = min(specHitMin, t);
      }
    }
#endif
  }
  diffuse /= float(spp);
  specular /= float(spp);
  coatSpec /= float(spp);

  // Luzes locais: o ponto de luz escolhido pelo ReSTIR (o mesmo testado e reaproveitado), com sombra.
  vec3 localLight = vec3(0.0);
  if (r.light != NO_LIGHT && r.W > 0.0) {
    vec3 kdl, ksl, L;
    float dist;
    bool lit = lightContribution(g, bc, r.light, r.uv, kdl, ksl, L, dist);
    if (lit && hair) {  // mesmo ponto de luz, BSDF do fio
      vec3 Le, Lh;
      float dh, Gh;
      lightPoint(r.light, r.uv, g.P, Lh, dh, Le, Gh);
      vec3 hk, hs;
      hairBSDF(hairT, g.N, g.V, Lh, hairColor, hairRough, hk, hs);
      kdl = hk * Le * Gh;
      ksl = hs * Le * Gh;
    }
    if (lit && (hair || dot(sf.Nb, L) > 0.0)) {
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
      if (coat > 0.0) {  // mesmo ponto de luz para o verniz
        vec3 Lc, Le, kdc, ksc;
        float distc, Gc;
        if (lightPoint(r.light, r.uv, g.P, Lc, distc, Le, Gc)) {
          evalBRDF(bcCoat, Lc, -1.0, kdc, ksc);
          coatSpec += ksc * Le * Gc * w;
        }
      }
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
  diffuse += bc.diffuseWeight * sampleSky(normalize(g.N + vec3(0, 1, 0))) * 0.8 * aoDiffuse;
  vec3 R = reflect(-g.V, g.N);
  specular += specularAlbedo(g.f0, g.f90, bc.dfg) * bc.energyCompensation *
              sampleSky(normalize(mix(R, g.N, g.roughness * g.roughness))) * aoSpecular;
#endif

  // Rede de segurança: um único NaN se espalharia pelo denoiser e pelo histórico do TAA.
  if (any(isnan(diffuse)) || any(isinf(diffuse))) diffuse = vec3(0.0);
  if (any(isnan(specular)) || any(isinf(specular))) specular = vec3(0.0);
  // Verniz por cima: a base recebe o que ele deixa passar; o reflexo dele soma no especular.
  diffuse *= coatAtten;
  specular = specular * coatAtten + coat * coatSpec;
  specular /= specularDemodulation(bcDemod);  // remultiplicado em composite.comp (mesmo contexto do G-buffer)
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
