// ReSTIR DI de produção. Base: Bitterli et al. 2020; reuso com pesos de MIS generalizados (GRIS, Lin et al.
// 2022) e MIS em pares no reuso espacial ("A Gentle Introduction to ReSTIR", Wyman et al. 2023).
//
// A amostra de um reservatório é um PONTO na superfície de uma luz (luz + coordenadas na esfera), em
// medida de área — o mesmo ponto é testado (visibilidade), sombreado e reaproveitado por vizinhos e pelo
// próximo frame. Assim descartar amostras ocultas não enviesa (o teste é exatamente o f(y) = 0 do
// sombreamento), e os vizinhos reaproveitam pontos, não "a luz inteira".
//
// Função-alvo p̂(y) = luminância de BRDF · cos · Le · G no ponto do pixel, sem visibilidade.
#ifndef RESTIR_GLSL
#define RESTIR_GLSL

#include "brdf.glsl"
#include "gbuffer.glsl"
#include "scene.glsl"
#include "light_bvh.glsl"

struct Reservoir {
  uint light;  // NO_LIGHT = vazio
  float W;     // peso de contribuição (≈ 1 / pdf do ponto escolhido)
  float M;     // confiança: quantos candidatos o reservatório resume
  float pHat;  // p̂ da amostra no próprio pixel (cache para os pesos de MIS dos vizinhos)
  vec2 uv;     // ponto na esfera da luz
  float pad0, pad1;
};

const uint NO_LIGHT = 0xFFFFFFFFu;
uint restirCandidates() { return max(frame.quality0.y, 1u); }  // candidatos iniciais por pixel (preset)
const float RESTIR_MAX_HISTORY = 20.0;  // limite de M temporal (em múltiplos dos candidatos iniciais)
bool restirBiasCorrection() { return frame.quality2.x != 0u; }  // raios de correção de viés no reuso (preset)

Reservoir emptyReservoir() { return Reservoir(NO_LIGHT, 0.0, 0.0, 0.0, vec2(0.0), 0.0, 0.0); }

BrdfContext brdfContextFor(GSample g) {
  return makeBrdfContext(g.N, g.V, g.f0, g.f90, g.roughness, energyCompensationEnabled());
}

// ---- Pontos na superfície das luzes esféricas ----
vec3 sphereDirFromUv(vec2 uv) {
  float z = 1.0 - 2.0 * uv.x, s = sqrt(max(1.0 - z * z, 0.0)), phi = TWO_PI * uv.y;
  return vec3(s * cos(phi), s * sin(phi), z);
}
vec2 sphereUvFromDir(vec3 n) { return vec2(0.5 * (1.0 - n.z), fract(atan(n.y, n.x) / TWO_PI + 1.0)); }

// Geometria do ponto (luz li, uv) visto de P. Falso se o ponto está de costas para P ou P está dentro da luz.
//   L, dist: direção e distância até o ponto; Le: radiância da superfície (com a janela de alcance);
//   G = cos no ponto da luz / dist² (converte área em ângulo sólido).
bool lightPoint(uint li, vec2 uv, vec3 P, out vec3 L, out float dist, out vec3 Le, out float G) {
  PointLight l = lights[li];
  if (l.type == LIGHT_TRIANGLE) {  // uv = baricêntricas do ponto no triângulo
    vec3 nRaw = cross(l.direction, l.edge2);
    vec3 d = l.position + uv.x * l.direction + uv.y * l.edge2 - P;
    float d2 = dot(d, d);
    dist = sqrt(d2);
    L = dist > 0.0 ? d / dist : vec3(0.0, 1.0, 0.0);
    Le = vec3(0.0);
    G = 0.0;
    if (dist <= 0.0 || dist >= l.range || dot(nRaw, nRaw) <= 0.0) return false;
    bool doubleSided;
    vec3 emitted = triangleLightRadiance(l, uv, doubleSided);
    float cosY = -dot(normalize(nRaw), L);
    if (doubleSided) cosY = abs(cosY);
    if (cosY <= 0.0) return false;
    Le = emitted * lightWindow(l, P, dist);
    G = cosY / d2;
    // O raio de sombra para antes do triângulo da própria luz com folga de 0,1%: com menos que isso ele esbarra nela
    // (medido no llvmpipe: com 0,001% sumia metade da luz de um globo; 0,01% ainda perdia 1%; 0,1% = sem sombra).
    dist *= 0.999;
    return dot(Le, Le) > 0.0;
  }
  float r = max(l.radius, 1e-3);
  float dc = distance(l.position, P);
  L = vec3(0.0, 1.0, 0.0);
  dist = 0.0;
  Le = vec3(0.0);
  G = 0.0;
  if (dc <= r || dc >= l.range) return false;
  vec3 n = sphereDirFromUv(uv);
  vec3 d = l.position + r * n - P;
  float d2 = dot(d, d);
  dist = sqrt(d2);
  L = d / dist;
  float cosY = -dot(n, L);
  if (cosY <= 0.0) return false;
  Le = l.intensity * lightWindow(l, P, dc) / (PI * r * r);
  G = cosY / d2;
  return true;
}

// Amostra um ponto da parte visível da esfera (cone uniforme em ângulo sólido) e devolve sua pdf em área.
bool sampleLightPoint(uint li, vec3 P, vec2 u, out vec2 uv, out float pdfArea) {
  PointLight l = lights[li];
  if (l.type == LIGHT_TRIANGLE) {  // uniforme na área (a face de costas é rejeitada no lightPoint)
    uv = triangleBarycentric(u);
    pdfArea = l.area > 0.0 ? 1.0 / l.area : 0.0;
    return pdfArea > 0.0;
  }
  float r = max(l.radius, 1e-3);
  vec3 toC = l.position - P;
  float d2c = dot(toC, toC);
  uv = vec2(0.0);
  pdfArea = 0.0;
  if (d2c <= r * r) return false;
  float dc = sqrt(d2c);
  float sinMax2 = r * r / d2c, cosMax = sqrt(max(0.0, 1.0 - sinMax2));
  float oneMinusCos = sinMax2 / (1.0 + cosMax);  // sem cancelamento em cones minúsculos
  vec3 L = sampleCone(toC / dc, cosMax, u);
  float b = dot(toC, L);
  float t = b - sqrt(max(b * b - (d2c - r * r), 0.0));
  vec3 n = normalize(P + L * t - l.position);
  uv = sphereUvFromDir(n);
  float cosY = -dot(n, L);
  if (cosY <= 1e-6) return false;
  pdfArea = cosY / (TWO_PI * oneMinusCos * t * t);  // dω = cos θ_y dA / d²  =>  p_A = p_ω cos θ_y / d²
  return true;
}

// Amostra uma luz para sombrear um ponto atingido por raio (luz direta fora do ReSTIR): direção, distância até a
// superfície da luz e iluminância equivalente E (estimador = BRDF × cos × E).
bool sampleLightForShading(uint li, vec3 P, vec2 u, out vec3 L, out float tSurface, out vec3 E) {
  PointLight l = lights[li];
  if (l.type != LIGHT_TRIANGLE) return sampleSphereLight(l, P, u, L, tSurface, E);
  vec2 uv = triangleBarycentric(u);
  vec3 Le;
  float G;
  E = vec3(0.0);
  if (!lightPoint(li, uv, P, L, tSurface, Le, G)) return false;
  E = Le * G * l.area;  // pdf em área = 1 / área
  return true;
}

// Contribuição sem sombra do ponto de luz em g: difuso demodulado (sem a cor difusa) e especular.
bool lightContribution(GSample g, BrdfContext bc, uint li, vec2 uv, out vec3 diffuse, out vec3 specular,
                       out vec3 L, out float dist) {
  vec3 Le;
  float G;
  diffuse = specular = vec3(0.0);
  if (li == NO_LIGHT || !lightPoint(li, uv, g.P, L, dist, Le, G)) return false;
  vec3 kd, ks;
  evalBRDF(bc, L, -1.0, kd, ks);
  diffuse = kd * Le * G;
  specular = ks * Le * G;
  return dot(diffuse + specular, vec3(1.0)) > 0.0;
}

float targetPdf(GSample g, BrdfContext bc, uint li, vec2 uv) {
  vec3 kd, ks, L;
  float dist;
  if (!lightContribution(g, bc, li, uv, kd, ks, L, dist)) return 0.0;
  return luminance(kd * g.diffuseColor + ks);
}

// ---- Reservatório em fluxo (streaming RIS) ----
struct ResamplingState {
  Reservoir r;
  float wSum;
};
ResamplingState beginResampling() { return ResamplingState(emptyReservoir(), 0.0); }

// w: peso de reamostragem; pHatCenter: p̂ do candidato no pixel que está reamostrando.
void streamSample(inout ResamplingState s, uint li, vec2 uv, float w, float pHatCenter, float M, float u) {
  s.r.M += M;
  if (!(w > 0.0)) return;
  s.wSum += w;
  if (u * s.wSum <= w) {
    s.r.light = li;
    s.r.uv = uv;
    s.r.pHat = pHatCenter;
  }
}

// W = (Σ w) / p̂(y): com pesos de MIS que somam 1 (GRIS) não se divide por M.
Reservoir finishResampling(ResamplingState s) {
  Reservoir r = s.r;
  r.W = (r.light != NO_LIGHT && r.pHat > 0.0) ? s.wSum / r.pHat : 0.0;
  if (r.W == 0.0) {
    r.light = NO_LIGHT;
    r.pHat = 0.0;
  }
  return r;
}

// Peso de MIS em pares (balance heuristic com confiança): fração de "a" contra "b".
float pairwiseMis(float pA, float MA, float pB, float MB) {
  float a = MA * pA, d = MA * pA + MB * pB;
  return d > 0.0 ? a / d : 0.0;
}

// ---- Filtro de "boiling" (como no RTXDI): reservatórios com peso muito acima da média do grupo de
// trabalho viram manchas que crescem com o reuso. Descarta os que passam de 10/força − 9 vezes a média. ----
const float BOILING_FILTER_STRENGTH = 0.2;
shared float boilingSum[64];
shared uint boilingCount[64];
void boilingFilter(uint localIndex, inout Reservoir r) {
  float weight = r.light != NO_LIGHT ? r.W * r.pHat : 0.0;
  boilingSum[localIndex] = weight;
  boilingCount[localIndex] = weight > 0.0 ? 1u : 0u;
  barrier();
  for (uint stride = 32u; stride > 0u; stride >>= 1u) {
    if (localIndex < stride) {
      boilingSum[localIndex] += boilingSum[localIndex + stride];
      boilingCount[localIndex] += boilingCount[localIndex + stride];
    }
    barrier();
  }
  float average = boilingCount[0] > 0u ? boilingSum[0] / float(boilingCount[0]) : 0.0;
  float multiplier = 10.0 / BOILING_FILTER_STRENGTH - 9.0;
  if (average > 0.0 && weight > average * multiplier) r = emptyReservoir();
}

#endif
