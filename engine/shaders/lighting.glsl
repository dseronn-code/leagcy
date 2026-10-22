// Iluminação deferred em compute. Gera dois sinais ruidosos para o denoiser:
//   difuso (demodulado: sem albedo, preserva textura) e especular.
// Com RAY_TRACING: sombras do sol, luz do ReSTIR com sombra, GI de 1 rebote e reflexões.
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

// Limita a luminância de amostras indiretas: troca um viés pequeno por muito menos "vagalumes"
// (amostras raras e muito brilhantes que o denoiser não consegue espalhar).
const float FIREFLY_LIMIT = 16.0;
vec3 clampFirefly(vec3 c) {
  float l = luminance(c);
  return l > FIREFLY_LIMIT ? c * (FIREFLY_LIMIT / l) : c;
}

#ifdef RAY_TRACING
// Radiância que chega ao longo de um raio: sol + 1 luz local aleatória + céu + emissão no ponto atingido.
vec3 traceRadiance(vec3 origin, vec3 dir, inout uint rng) {
  HitInfo hit;
  if (!traceHit(origin, dir, 1e4, hit)) return sampleSky(dir);
  Material m = materials[hit.material];
  Surface s = evalMaterial(m, hit.P, hit.N, vec4(0.0), hit.uv);
  vec3 diffuse = s.albedo * (1.0 - s.metallic) / PI;
  vec3 origin2 = hit.P + hit.N * RAY_EPS;
  vec3 radiance = s.emissive;

  vec3 sunE = sunIrradianceAt(hit.P);
  vec3 Ls = sampleCone(frame.sunDir.xyz, cos(frame.params.x), rand2(rng));
  float NoLs = dot(hit.N, Ls);
  if (NoLs > 0.0 && dot(sunE, sunE) > 0.0) radiance += diffuse * NoLs * sunE * traceVisibility(origin2, Ls, 1e4);

  uint lightCount = frame.frameInfo.z;
  if (lightCount > 0u) {
    PointLight l = lights[min(uint(rand(rng) * float(lightCount)), lightCount - 1u)];
    vec3 Lv = l.position - hit.P;
    float dist = length(Lv);
    float NoL = dot(hit.N, Lv / dist);
    if (NoL > 0.0 && dist < l.range)
      radiance += diffuse * NoL * lightRadiance(l, dist) * float(lightCount) * traceVisibility(origin2, Lv / dist, dist - l.radius);
  }
  // Rebotes seguintes aproximados pelo céu.
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
  uint rng = (uint(id.x) * 1973u + uint(id.y) * 9277u + frame.frameInfo.x * 26699u + 7u) | 1u;
  vec3 diffuse = vec3(0.0), specular = vec3(0.0), kd, ks;
  vec3 origin = g.P + g.N * 0.01;

  // Sol: disco amostrado => penumbra física.
  vec3 sunE = sunIrradianceAt(g.P);
  if (dot(sunE, sunE) > 0.0) {
    vec3 L = sampleCone(frame.sunDir.xyz, cos(frame.params.x), rand2(rng));
    evalBRDFSplit(g.N, g.V, L, g.f0, g.roughness, kd, ks);
#ifdef RAY_TRACING
    float vis = dot(g.N, L) > 0.0 ? traceVisibility(origin, L, 1e4) : 0.0;
#else
    float vis = 1.0;
#endif
    diffuse += kd * sunE * vis;
    specular += ks * sunE * vis;
  }

  // Luzes locais: a amostra escolhida pelo ReSTIR, ponderada por W.
  Reservoir r = reservoirs[uint(id.y) * uint(frame.renderSize.x) + uint(id.x)];
  if (r.light != NO_LIGHT && r.W > 0.0) {
    PointLight l = lights[r.light];
    vec3 lp = l.position + (rand2(rng).xyx - 0.5) * l.radius;  // ponto na luz (sombra suave)
    vec3 Lv = lp - g.P;
    float dist = length(Lv);
    vec3 L = Lv / dist;
    evalBRDFSplit(g.N, g.V, L, g.f0, g.roughness, kd, ks);
#ifdef RAY_TRACING
    float vis = traceVisibility(origin, L, max(dist - l.radius, 0.0));
#else
    float vis = 1.0;
#endif
    vec3 Li = lightRadiance(l, dist) * r.W * vis;
    diffuse += kd * Li;
    specular += ks * Li;
  }

  // Normal map pode virar a normal de costas para a câmera: corrige para o hemisfério visível.
  float NoVraw = dot(g.N, g.V);
  if (NoVraw < 1e-3) {
    g.N = normalize(g.N + g.V * (1e-3 - NoVraw));
    NoVraw = dot(g.N, g.V);
  }
  float NoV = max(NoVraw, 1e-4);
  vec3 Fv = F_Schlick(g.f0, NoV);
#ifdef RAY_TRACING
  // GI difusa: amostragem por cosseno => estimador = (1 - F) * Li.
  diffuse += (1.0 - Fv) * traceRadiance(origin, cosineSampleHemisphere(g.N, rand2(rng)), rng);

  // Especular: amostra VNDF => peso = F * G1(L).
  float a = max(g.roughness * g.roughness, 0.002);
  vec3 T, B;
  orthonormalBasis(g.N, T, B);
  vec3 Ht = sampleGGXVNDF(vec3(dot(g.V, T), dot(g.V, B), dot(g.V, g.N)), a, rand2(rng));
  vec3 H = dot(Ht, Ht) > 0.0 ? T * Ht.x + B * Ht.y + g.N * Ht.z : g.N;
  vec3 Lr = reflect(-g.V, H);
  float NoLr = dot(g.N, Lr);
  if (NoLr > 0.0) specular += traceRadiance(origin, Lr, rng) * F_Schlick(g.f0, max(dot(g.V, H), 0.0)) * G1_Smith(NoLr, a);
#else
  diffuse += (1.0 - Fv) * sampleSky(normalize(g.N + vec3(0, 1, 0))) * 0.8;
  vec3 R = reflect(-g.V, g.N);
  specular += Fv * sampleSky(normalize(mix(R, g.N, g.roughness * g.roughness))) * (1.0 - 0.7 * g.roughness);
#endif

  // Rede de segurança: um único NaN se espalharia pelo denoiser e pelo histórico do TAA.
  if (any(isnan(diffuse)) || any(isinf(diffuse))) diffuse = vec3(0.0);
  if (any(isnan(specular)) || any(isinf(specular))) specular = vec3(0.0);
  imageStore(outDiffuse, id, vec4(diffuse, 1.0));
  imageStore(outSpecular, id, vec4(specular, 1.0));
}
