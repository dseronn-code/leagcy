// BRDF de produção: GGX + Smith height-correlated + Fresnel-Schlick com f90, compensação de múltiplo
// espalhamento (Turquin 2019) e difuso com "albedo scaling" (a energia refletida pelo especular sai do
// difuso, como no OpenPBR). A LUT DFG (set 0, binding 9) guarda a integral do GGX: x = A, y = B, com
// albedo direcional E(μ) = f0·A + f90·B.
#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "common.glsl"

layout(set = 0, binding = 9) uniform sampler2D dfgLut;  // u = NoV, v = rugosidade perceptual

float D_GGX(float NoH, float a) {
  float a2 = a * a;
  float d = (NoH * a2 - NoH) * NoH + 1.0;
  return a2 / (PI * d * d);
}
float V_SmithGGXCorrelated(float NoV, float NoL, float a) {
  float a2 = a * a;
  float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
  float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
  return 0.5 / max(gv + gl, 1e-6);
}
vec3 F_Schlick(vec3 f0, float f90, float VoH) {
  float f = pow(1.0 - VoH, 5.0);
  return f0 + (vec3(f90) - f0) * f;
}
float G1_Smith(float NoX, float a) {
  float a2 = a * a;
  return 2.0 * NoX / (NoX + sqrt(a2 + (1.0 - a2) * NoX * NoX));
}
// Peso de uma amostra VNDF (sem o Fresnel): G2(V, L) / G1(V) com o G2 correlacionado em altura — o mesmo BRDF que
// evalBRDF avalia na amostragem de luzes. Antes usava G1(L), que é o G2 não correlacionado: os raios especulares
// pesavam um pouco mais que a luz direta (medido: ~2% a mais na prova das luzes de malha, cena mesh-lights-panel).
float vndfWeight(float NoV, float NoL, float a) {
  return V_SmithGGXCorrelated(NoV, NoL, a) * 4.0 * NoV * NoL / max(G1_Smith(NoV, a), 1e-6);
}
float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Rugosidade perceptual mínima: abaixo disso o GGX vira um pico que nenhum filtro resolve.
const float MIN_ROUGHNESS = 0.03;
float alphaFromRoughness(float roughness) { return max(roughness * roughness, MIN_ROUGHNESS * MIN_ROUGHNESS); }

vec2 sampleDFG(float NoV, float roughness) { return textureLod(dfgLut, vec2(NoV, roughness), 0.0).xy; }

// Albedo direcional do lóbulo especular (fração da luz refletida pelo especular).
vec3 specularAlbedo(vec3 f0, float f90, vec2 dfg) { return f0 * dfg.x + f90 * dfg.y; }

// Turquin 2019: o GGX de espalhamento único perde a energia que quica entre microfacetas (até ~60%
// em metais ásperos). O fator devolve essa energia proporcionalmente a f0.
vec3 specularEnergyCompensation(vec3 f0, vec2 dfg) {
  float ess = max(dfg.x + dfg.y, 1e-4);
  return 1.0 + f0 * (1.0 / ess - 1.0);
}

struct BrdfContext {
  vec3 N, V;
  vec3 f0;
  float f90;
  float roughness;  // perceptual
  float NoV;
  vec2 dfg;
  vec3 energyCompensation;
  vec3 diffuseWeight;  // 1 - albedo do especular na direção de visão
};

// Refletância especular esperada na direção de visão (DFG × compensação de energia). O especular vai para
// o denoiser dividido por ela e é multiplicado de volta na composição ("demodulação", como o NRD pede):
// assim o filtro espacial não borra o detalhe das texturas de f0 junto com o ruído.
vec3 specularDemodulation(BrdfContext c) { return max(specularAlbedo(c.f0, c.f90, c.dfg) * c.energyCompensation, vec3(0.01)); }

BrdfContext makeBrdfContext(vec3 N, vec3 V, vec3 f0, float f90, float roughness, bool energyCompensation) {
  BrdfContext c;
  c.N = N;
  c.V = V;
  c.f0 = f0;
  c.f90 = f90;
  c.roughness = roughness;
  c.NoV = max(dot(N, V), 1e-4);
  c.dfg = sampleDFG(c.NoV, roughness);
  c.energyCompensation = energyCompensation ? specularEnergyCompensation(f0, c.dfg) : vec3(1.0);
  c.diffuseWeight = energyCompensation ? max(1.0 - specularAlbedo(f0, f90, c.dfg) * c.energyCompensation, 0.0)
                                       : max(1.0 - F_Schlick(f0, f90, c.NoV), 0.0);
  return c;
}

// Resposta a uma direção de luz L: difuso "demodulado" (sem a cor difusa, para o denoiser) e especular.
// Ambos já incluem NoL. `roughnessOverride` < 0 usa a rugosidade do contexto (o sol usa uma maior).
void evalBRDF(BrdfContext c, vec3 L, float roughnessOverride, out vec3 diffuse, out vec3 specular) {
  float NoL = dot(c.N, L);
  diffuse = specular = vec3(0.0);
  if (NoL <= 0.0) return;
  vec3 H = normalize(c.V + L);
  float NoH = max(dot(c.N, H), 0.0), VoH = max(dot(c.V, H), 0.0);
  float a = alphaFromRoughness(roughnessOverride >= 0.0 ? roughnessOverride : c.roughness);
  vec3 F = F_Schlick(c.f0, c.f90, VoH);
  specular = D_GGX(NoH, a) * V_SmithGGXCorrelated(c.NoV, NoL, a) * F * NoL * c.energyCompensation;
  diffuse = c.diffuseWeight / PI * NoL;
}

// Amostragem VNDF por calotas esféricas (Dupuy & Benyoub 2023), Ve em espaço tangente.
vec3 sampleGGXVNDF(vec3 Ve, float a, vec2 u) {
  vec3 Vh = normalize(vec3(a * Ve.x, a * Ve.y, Ve.z));
  float phi = TWO_PI * u.x;
  float z = (1.0 - u.y) * (1.0 + Vh.z) - Vh.z;
  float sinT = sqrt(clamp(1.0 - z * z, 0.0, 1.0));
  vec3 Nh = vec3(sinT * cos(phi), sinT * sin(phi), z) + Vh;
  return normalize(vec3(a * Nh.x, a * Nh.y, max(0.0, Nh.z)));
}

#endif
