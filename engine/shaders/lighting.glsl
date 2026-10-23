// Iluminação deferred em compute. Gera dois sinais ruidosos para o denoiser:
//   difuso (demodulado: sem albedo, preserva textura) e especular.
// Com RAY_TRACING: sombras do sol, luz do ReSTIR com sombra, GI de 1 rebote e reflexões.
// Toda radiância sai pré-exposta (ver frame.glsl).
#extension GL_GOOGLE_include_directive : require
#include "restir.glsl"
#define MATERIAL_SAMPLE(i, uv) textureLod(textures[nonuniformEXT(i)], uv, 2.0)
#include "material.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 3, std430) readonly buffer Reservoirs { Reservoir reservoirs[]; };
layout(set = 1, binding = 4, rgba16f) uniform writeonly image2D outDiffuse;
layout(set = 1, binding = 5, rgba16f) uniform writeonly image2D outSpecular;

// Dimensões da sequência de Sobol por pixel (cada uma é um par 2D independente).
const uint DIM_SUN = 0u, DIM_LIGHT = 1u, DIM_DIFFUSE = 2u, DIM_SPECULAR = 3u;
const uint DIM_HIT_SUN = 4u, DIM_HIT_LIGHT = 5u, DIM_HIT_LIGHT_PICK = 6u;

// Limita a luminância de amostras indiretas: troca um viés pequeno por muito menos "vagalumes"
// (amostras raras e muito brilhantes que o denoiser não consegue espalhar). Valor pré-exposto:
// 16 ≈ 90x o cinza médio na tela.
const float FIREFLY_LIMIT = 16.0;
vec3 clampFirefly(vec3 c) {
  float l = luminance(c);
  return l > FIREFLY_LIMIT ? c * (FIREFLY_LIMIT / l) : c;
}

#ifdef RAY_TRACING
// Radiância que chega ao longo de um raio: sol + 1 luz local aleatória + céu + emissão no ponto atingido.
// `bounce` separa as dimensões de Sobol do raio difuso e do especular.
vec3 traceRadiance(vec3 origin, vec3 dir, SampleGen sg, uint bounce) {
  HitInfo hit;
  if (!traceHit(origin, dir, 1e4, hit)) return sampleSky(dir);
  Material m = materials[hit.material];
  Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv);
  vec3 diffuse = s.albedo * (1.0 - s.metallic) / PI;
  vec3 origin2 = offsetRayOrigin(hit.P, hit.Ng, hit.t);
  vec3 radiance = s.emissive * preExposure();
  uint dimBase = bounce * 3u;

  vec3 sunE = sunIrradianceAt(hit.P);
  vec3 Ls = sampleCone(frame.sunDir.xyz, cos(frame.params.x), sample2D(sg, DIM_HIT_SUN + dimBase));
  float NoLs = dot(hit.N, Ls);
  if (NoLs > 0.0 && dot(hit.Ng, Ls) > 0.0 && dot(sunE, sunE) > 0.0)
    radiance += diffuse * NoLs * sunE * traceVisibility(origin2, Ls, 1e4);

  uint lightCount = frame.frameInfo.z;
  if (lightCount > 0u) {
    float pick = sample2D(sg, DIM_HIT_LIGHT_PICK + dimBase).x;
    PointLight l = lights[min(uint(pick * float(lightCount)), lightCount - 1u)];
    vec3 L, E;
    float tSurface;
    if (sampleSphereLight(l, hit.P, sample2D(sg, DIM_HIT_LIGHT + dimBase), L, tSurface, E)) {
      float NoL = dot(hit.N, L);
      if (NoL > 0.0 && dot(hit.Ng, L) > 0.0 && dot(E, E) > 0.0)
        radiance += diffuse * NoL * E * preExposure() * float(lightCount) * traceVisibility(origin2, L, tSurface);
    }
  }
  // Rebotes seguintes aproximados pelo céu (sem oclusão — auditoria 3.1).
  radiance += s.albedo * (1.0 - s.metallic) * sampleSky(normalize(hit.N + vec3(0, 1, 0))) * 0.5;
  return clampFirefly(radiance);
}
#endif

void main() {
  ivec2 id = ivec2(gl_GlobalInvocationID.xy);
  if (!insideRender(id)) return;
  GSample g = loadGBuffer(id);
  if (g.depth <= 0.0) {
    imageStore(outDiffuse, id, vec4(0.0));
    imageStore(outSpecular, id, vec4(0.0));
    return;
  }
  SampleGen sg = makeSampleGen(uvec2(id), frame.frameInfo.x);
  vec3 diffuse = vec3(0.0), specular = vec3(0.0), kd, ks;
#ifdef RAY_TRACING
  vec3 origin = offsetRayOrigin(g.P, g.Ng, length(frame.cameraPos.xyz - g.P));
#endif

  // Sol como disco de verdade: o BRDF usa a direção central com a rugosidade alargada pelo tamanho
  // angular do disco (Karis 2013) — sem o pico de um sol pontual, que gera brilhinhos. Só a
  // visibilidade usa uma direção sorteada no disco (penumbra física).
  vec3 sunE = sunIrradianceAt(g.P);
  if (dot(sunE, sunE) > 0.0) {
    float sunRoughness = sqrt(min(g.roughness * g.roughness + 0.5 * frame.params.x, 1.0));
    evalBRDFSplit(g.N, g.V, frame.sunDir.xyz, g.f0, sunRoughness, kd, ks);
#ifdef RAY_TRACING
    vec3 L = sampleCone(frame.sunDir.xyz, cos(frame.params.x), sample2D(sg, DIM_SUN));
    float vis = dot(g.Ng, L) > 0.0 ? traceVisibility(origin, L, 1e4) : 0.0;
#else
    float vis = 1.0;
#endif
    diffuse += kd * sunE * vis;
    specular += ks * sunE * vis;
  }

  // Luzes locais: a luz escolhida pelo ReSTIR, integrada sobre sua esfera (ângulo sólido).
  Reservoir r = reservoirs[uint(id.y) * uint(frame.renderSize.x) + uint(id.x)];
  if (r.light != NO_LIGHT && r.W > 0.0) {
    PointLight l = lights[r.light];
    vec3 L, E;
    float tSurface;
    if (sampleSphereLight(l, g.P, sample2D(sg, DIM_LIGHT), L, tSurface, E)) {
      evalBRDFSplit(g.N, g.V, L, g.f0, g.roughness, kd, ks);
#ifdef RAY_TRACING
      float vis = dot(g.Ng, L) > 0.0 ? traceVisibility(origin, L, tSurface) : 0.0;
#else
      float vis = 1.0;
#endif
      vec3 Li = E * preExposure() * r.W * vis;
      diffuse += kd * Li;
      specular += ks * Li;
    }
  }

  float NoV = max(dot(g.N, g.V), 1e-4);
  vec3 Fv = F_Schlick(g.f0, NoV);
#ifdef RAY_TRACING
  // GI difusa: amostragem por cosseno => estimador = (1 - F) * Li. Direções abaixo da faceta
  // geométrica (possíveis com normal map) atravessariam a superfície: são descartadas.
  vec3 dirD = cosineSampleHemisphere(g.N, sample2D(sg, DIM_DIFFUSE));
  if (dot(dirD, g.Ng) > 0.0) diffuse += (1.0 - Fv) * traceRadiance(origin, dirD, sg, 0u);

  // Especular: amostra VNDF => peso = F * G1(L).
  float a = max(g.roughness * g.roughness, 0.002);
  vec3 T, B;
  orthonormalBasis(g.N, T, B);
  vec3 Ht = sampleGGXVNDF(vec3(dot(g.V, T), dot(g.V, B), dot(g.V, g.N)), a, sample2D(sg, DIM_SPECULAR));
  vec3 H = dot(Ht, Ht) > 0.0 ? T * Ht.x + B * Ht.y + g.N * Ht.z : g.N;
  vec3 Lr = reflect(-g.V, H);
  float NoLr = dot(g.N, Lr);
  if (NoLr > 0.0 && dot(Lr, g.Ng) > 0.0)
    specular += traceRadiance(origin, Lr, sg, 1u) * F_Schlick(g.f0, max(dot(g.V, H), 0.0)) * G1_Smith(NoLr, a);
#else
  diffuse += (1.0 - Fv) * sampleSky(normalize(g.N + vec3(0, 1, 0))) * 0.8;
  vec3 R = reflect(-g.V, g.N);
  specular += Fv * sampleSky(normalize(mix(R, g.N, g.roughness * g.roughness))) * (1.0 - 0.7 * g.roughness);
#endif

  // Rede de segurança: um único NaN se espalharia pelo denoiser e pelo histórico do TAA.
  if (any(isnan(diffuse)) || any(isinf(diffuse))) diffuse = vec3(0.0);
  if (any(isnan(specular)) || any(isinf(specular))) specular = vec3(0.0);
  imageStore(outDiffuse, id, vec4(clampHalf(diffuse), 1.0));
  imageStore(outSpecular, id, vec4(clampHalf(specular), 1.0));
}
