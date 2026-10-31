// Dados globais do frame (set 0, bindings 0-1) — espelha FrameUniforms em renderer.cpp.
#ifndef FRAME_GLSL
#define FRAME_GLSL

#include "common.glsl"
#include "atmosphere.glsl"

layout(set = 0, binding = 0) uniform FrameData {
  mat4 view;
  mat4 proj;                  // com jitter de subpixel (TAA)
  mat4 viewProj;              // com jitter
  mat4 invViewProj;           // com jitter, RELATIVA À CÂMERA: NDC -> (posição - câmera); ver worldPosFromDepth
  mat4 viewProjNoJitter;
  mat4 prevViewProjNoJitter;
  mat4 prevInvViewProj;       // do frame anterior, com o jitter dele, relativa à câmera anterior
  vec4 cameraPos;             // xyz = posição (m), w = near plane
  vec4 prevCameraPos;
  vec4 sunDir;                // xyz = direção PARA o sol, w = iluminância solar no topo da atmosfera (lux)
  vec4 params;                // x = raio angular do sol, y = dt (s), z = densidade da névoa, w = queda da névoa
  vec4 jitter;                // xy = deslocamento do jitter em UV, zw = do frame anterior
  vec4 renderSize;            // xy = resolução interna, zw = 1/xy
  vec4 outputSize;            // xy = resolução de saída, zw = 1/xy
  uvec4 frameInfo;            // x = índice do frame, y = ray tracing, z = nº de luzes, w = histórico válido
  uvec4 quality0;             // x = amostras por pixel, y = candidatos ReSTIR, z = vizinhos ReSTIR, w = amostras do DOF
  vec4 quality1;              // x = mip das texturas nos raios, y = limite de vagalumes, z = amostras do motion blur,
                              // w = compensação de energia do BRDF (1 = ligada)
  uvec4 quality2;             // x = correção de viés do ReSTIR por ray tracing (1 = ligada),
                              // y = difuso/especular no formato do NRD ReBLUR (YCoCg + distância normalizada),
                              // z = mip bias das texturas na rasterização (bits de float; < 0 com DLSS/FSR),
                              // w = bit 0: referência por path tracing (rebotes de verdade); bit 1: cache de radiância desligado;
                              //     bit 2: movimento virtual do reflexo desligado no SVGF (medição)
  vec4 environment;           // x = céu uniforme em nits (> 0: teste da fornalha branca)
  vec4 night;                 // x = 1: a luz direcional (sunDir) é a lua; y = iluminância de magnitude 0 (lux);
                              // z = cobertura de nuvens (0 = céu limpo); w = tempo (s, move as nuvens)
  vec4 weather;               // x = superfícies molhadas, y = poças, z = chuva caindo (respingos),
                              // w = tempo da cena no frame anterior (s; o atual é night.w)
  mat4 shadowMatrices[4];     // shadow maps em cascata: mundo -> clip de cada cascata (vista do sol)
  vec4 shadowSplits;          // xyz = fim das cascatas 0..2 (m da câmera), w = fim da última; 0 = desligado
  vec4 shadowParams;          // x = metros por texel da cascata 0, y = 1: usa os shadow maps mesmo com ray tracing
  vec4 water;                 // x = nível do mar (m), y = 1: há oceano
  vec4 ripple;                // ondulações (ripple.comp): xy = canto da grade (m), z = tamanho (m), w = 1 ligadas
} frame;

layout(set = 0, binding = 23) uniform sampler2D rippleHeight;  // r = altura das ondulações (m)

// Inclinação (∂h/∂x, ∂h/∂z) das ondulações em P, some na borda da grade.
vec2 rippleSlope(vec3 P) {
  if (frame.ripple.w <= 0.0) return vec2(0.0);
  vec2 uv = (P.xz - frame.ripple.xy) / frame.ripple.z;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return vec2(0.0);
  float texel = 1.0 / float(textureSize(rippleHeight, 0).x), cell = frame.ripple.z * texel;
  float hx = textureLod(rippleHeight, uv + vec2(texel, 0.0), 0.0).r - textureLod(rippleHeight, uv - vec2(texel, 0.0), 0.0).r;
  float hz = textureLod(rippleHeight, uv + vec2(0.0, texel), 0.0).r - textureLod(rippleHeight, uv - vec2(0.0, texel), 0.0).r;
  return vec2(hx, hz) / (2.0 * cell);
}

// Água do mar (Jerlov tipo I, oceano claro): absorção + espalhamento por metro; o vermelho some em poucos metros.
const vec3 SEA_EXTINCTION = vec3(0.45, 0.065, 0.045);
const vec3 SEA_SCATTER_ALBEDO = vec3(0.05, 0.55, 0.6);
bool underwater(vec3 p) { return frame.water.y > 0.5 && p.y < frame.water.x; }

// Simulação do oceano (renderer/ocean.cpp): repete a cada OCEAN_PATCH metros.
layout(set = 0, binding = 17) uniform sampler2D oceanDisplacement;  // xyz = deslocamento, w = Jacobiano
layout(set = 0, binding = 18) uniform sampler2D oceanSlope;         // xy = inclinação, z = espuma
const float OCEAN_PATCH = 256.0;

// Cáusticas no fundo: as ondas da FFT focam a luz onde a superfície se comprime (Jacobiano < 1). A luz que chega a
// um ponto a d metros de profundidade passou pela superfície no ponto acima dele, deslocado na direção do sol.
float seaCaustics(vec3 p) {
  float depth = frame.water.x - p.y;
  vec2 s = p.xz + frame.sunDir.xz / max(frame.sunDir.y, 0.2) * depth;  // onde o raio de sol cruzou a superfície
  float J = textureLod(oceanDisplacement, s / OCEAN_PATCH, 0.0).w;
  // Foco cresce com a profundidade até ~10 m e depois se desfaz (as cáusticas borram com a distância).
  float focus = clamp(depth / 6.0, 0.0, 1.0) * exp(-depth / 30.0);
  return clamp(1.0 + (1.0 / max(J, 0.25) - 1.0) * 1.8 * focus, 0.2, 4.0);
}

// Shadow maps em cascata (sem ray tracing, ou --shadow-maps): visibilidade do sol num ponto, com PCF.
layout(set = 0, binding = 21) uniform sampler2DArrayShadow shadowCascades;
bool shadowMapsEnabled() { return frame.shadowSplits.w > 0.0; }
float sunShadowMap(vec3 P, vec3 Ng) {
  float dist = distance(P, frame.cameraPos.xyz);
  if (dist >= frame.shadowSplits.w) return 1.0;  // além da última cascata: sem sombra
  int c = dist < frame.shadowSplits.x ? 0 : dist < frame.shadowSplits.y ? 1 : dist < frame.shadowSplits.z ? 2 : 3;
  // Texel cresce com a cascata (cada uma cobre mais); deslocamento pela normal contra a "acne".
  float texel = frame.shadowParams.x * pow(2.6, float(c));
  vec3 p = P + Ng * texel * 1.5;
  vec4 clip = frame.shadowMatrices[c] * vec4(p, 1.0);
  vec3 s = clip.xyz / clip.w;
  vec2 uv = s.xy * 0.5 + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
  float d = s.z - 0.0005;
  vec2 ts = 1.0 / vec2(textureSize(shadowCascades, 0).xy);
  float sum = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)  // textureGrad: vale também em compute (sem derivadas implícitas)
      sum += textureGrad(shadowCascades, vec4(uv + vec2(x, y) * ts * 1.5, float(c), d), vec2(0.0), vec2(0.0));
  return sum / 9.0;
}

// Sombra das nuvens (cloud_shadow.comp): transmitância da camada na direção do sol, num quadrado em volta da câmera.
layout(set = 0, binding = 16) uniform sampler2D cloudShadowMap;
const float CLOUD_SHADOW_EXTENT = 40000.0;  // m

// Oclusão da chuva (rain_occlusion.comp): altura da superfície exposta ao céu, num quadrado em volta da câmera.
layout(set = 0, binding = 20) uniform sampler2D rainOcclusionMap;
const float RAIN_EXTENT = 160.0;  // m
// Centro do mapa preso a uma grade de 1 m (o mapa não "nada" quando a câmera anda).
vec2 rainMapCenter() { return floor(frame.cameraPos.xz); }
// Chuva caindo (0..1). weather.z < 0 codifica "sem mapa de oclusão" (sem ray tracing): −1 − chuva.
float rainIntensity() { return frame.weather.z < 0.0 ? -frame.weather.z - 1.0 : frame.weather.z; }
// 1 = exposto à chuva, 0 = abrigado. Fora do mapa (longe) ou sem mapa: exposto.
float rainExposure(vec3 p) {
  if (frame.weather.z < 0.0) return 1.0;
  vec2 uv = (p.xz - rainMapCenter()) / RAIN_EXTENT + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
  // Rampa de 30 cm: a borda do abrigo não fica serrilhada pelas células de 60 cm.
  return smoothstep(-0.3, 0.0, p.y - textureLod(rainOcclusionMap, uv, 0.0).r);
}

bool energyCompensationEnabled() { return frame.quality1.w > 0.5; }
bool reblurSignal() { return frame.quality2.y != 0u; }
float textureMipBias() { return uintBitsToFloat(frame.quality2.z); }
bool pathReference() { return (frame.quality2.w & 1u) != 0u; }
bool radianceCacheEnabled() { return (frame.quality2.w & 2u) == 0u; }
bool virtualMotionEnabled() { return (frame.quality2.w & 4u) == 0u; }

// Conversões YCoCg do NRD (_NRD_LinearToYCoCg / _NRD_YCoCgToLinear).
vec3 nrdLinearToYCoCg(vec3 c) { return vec3(dot(c, vec3(0.25, 0.5, 0.25)), dot(c, vec3(0.5, 0.0, -0.5)), dot(c, vec3(-0.25, 0.5, -0.25))); }
vec3 nrdYCoCgToLinear(vec3 c) {
  float t = c.x - c.z;
  return max(vec3(t + c.y, c.x + c.z, t - c.y), vec3(0.0));
}

layout(set = 0, binding = 1) uniform sampler2D skyViewLut;

// Exposição (Frostbite/UE): toda radiância é guardada já multiplicada pela exposição do frame
// anterior ("pré-exposição"), mantendo valores de 120.000 lux a 0,001 nit dentro do float16.
// x = exposição mais recente (= pré-exposição deste frame), w = pré-exposição do frame anterior.
layout(set = 0, binding = 8, std430) readonly buffer GlobalExposure { vec4 exposureState; };
float preExposure() { return exposureState.x; }
// Fator para trazer históricos (denoiser, TAA) gravados no frame anterior para a escala atual.
float historyExposureScale() { return exposureState.w > 0.0 ? exposureState.x / exposureState.w : 1.0; }
// Maior valor seguro num alvo float16 (65504), com folga para filtros que somam vizinhos.
const float HALF_SAFE_MAX = 60000.0;
vec3 clampHalf(vec3 c) { return min(c, vec3(HALF_SAFE_MAX)); }

#include "atmosphere_lut.glsl"

bool historyValid() { return frame.frameInfo.w != 0u; }

// Reverse-Z infinito: profundidade = near / distância.
float linearDepth(float d) { return frame.cameraPos.w / max(d, 1e-7); }

// Posição relativa à câmera primeiro, câmera somada no fim: com a matriz absoluta, longe da origem (mundo aberto)
// o ponto perto da câmera saía com erro de ulp(coordenada) — a 4 km, 0,2 mm a 20 cm da câmera = direção 1e-3 rad
// torta (3 cm de erro a 30 m). Medido: luz perdida no terminator a 4 km de 2,4% (lavapipe) / 9,4% (RTX 3070 Ti).
vec3 cameraRelativePosFromDepth(vec2 uv, float depth) {
  vec4 p = frame.invViewProj * vec4(uv * 2.0 - 1.0, depth, 1.0);
  return p.xyz / p.w;
}
vec3 worldPosFromDepth(vec2 uv, float depth) { return frame.cameraPos.xyz + cameraRelativePosFromDepth(uv, depth); }
vec3 viewDirFromUv(vec2 uv) { return normalize(cameraRelativePosFromDepth(uv, 0.5)); }

// Posição em km relativa ao centro do planeta.
vec3 planetPos(vec3 worldMeters) {
  return vec3(0.0, R_GROUND + max(worldMeters.y * 0.001, 0.0) + 0.002, 0.0);
}

// Sky-view LUT (Hillaire 2020): azimute linear, latitude não linear (resolução no horizonte).
vec2 skyLutUv(vec3 dir) {
  float az = atan(dir.z, dir.x);
  float lat = asin(clamp(dir.y, -1.0, 1.0));
  float v = 0.5 + 0.5 * sign(lat) * sqrt(abs(lat) / (0.5 * PI));
  return vec2(fract(az / TWO_PI), v);
}
vec3 lutDirection(vec2 uv) {
  float az = uv.x * TWO_PI;
  float vv = uv.y * 2.0 - 1.0;
  float lat = sign(vv) * vv * vv * 0.5 * PI;
  return vec3(cos(lat) * cos(az), sin(lat), cos(lat) * sin(az));
}

// Radiância do céu (nits), já pré-exposta: a LUT é escrita com a pré-exposição do frame.
vec3 sampleSky(vec3 dir) { return textureLod(skyViewLut, skyLutUv(dir), 0.0).rgb; }

// Fração da luz do sol que passa pelas nuvens até o ponto: o ponto é levado ao chão ao longo da direção do sol.
float cloudShadow(vec3 worldPos) {
  if (frame.night.z <= 0.0) return 1.0;
  vec3 L = frame.sunDir.xyz;
  vec2 ground = worldPos.xz - L.xz / max(L.y, 0.03) * max(worldPos.y, 0.0);
  vec2 uv = (ground - frame.cameraPos.xz) / CLOUD_SHADOW_EXTENT + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
  return textureLod(cloudShadowMap, uv, 0.0).r;
}

// Iluminância do sol (lux) chegando ao ponto, já pré-exposta (atmosfera, nuvens e, debaixo d'água, a absorção no
// caminho até a superfície e as cáusticas).
vec3 sunIrradianceAt(vec3 worldPos) {
  if (frame.sunDir.w <= 0.0) return vec3(0.0);
  vec3 E = frame.sunDir.w * preExposure() * transmittanceToSun(planetPos(worldPos), frame.sunDir.xyz) * cloudShadow(worldPos);
  if (underwater(worldPos)) {
    float path = (frame.water.x - worldPos.y) / max(frame.sunDir.y, 0.1);
    E *= 0.98 * exp(-SEA_EXTINCTION * path) * seaCaustics(worldPos);  // 2% refletidos na superfície
  }
  return E;
}

vec3 sunDisk(vec3 dir) {
  float cosR = cos(frame.params.x);
  float mu = dot(dir, frame.sunDir.xyz);
  if (mu <= cosR || frame.sunDir.w <= 0.0) return vec3(0.0);
  float r = sqrt(max(0.0, 1.0 - mu * mu)) / sin(frame.params.x);
  float limb = 1.0 - 0.6 * (1.0 - sqrt(max(0.0, 1.0 - r * r)));  // escurecimento de borda
  if (frame.night.x > 0.5) {
    // Lua: sem escurecimento de borda (superfície que espalha) e com os "mares" escuros, por ruído de baixa frequência
    // num referencial preso ao disco.
    vec3 t, b;
    orthonormalBasis(frame.sunDir.xyz, t, b);
    vec2 p = vec2(dot(dir, t), dot(dir, b)) / sin(frame.params.x);
    limb = 1.15 - 0.45 * smoothstep(0.35, 0.75, fbm(p * 2.2 + 3.7, 4));
  }
  // Sem a sombra das nuvens: quem desenha o disco aplica a transmitância das nuvens na direção do pixel.
  vec3 E = frame.sunDir.w * preExposure() * transmittanceToSun(planetPos(frame.cameraPos.xyz), frame.sunDir.xyz);
  return E / (TWO_PI * (1.0 - cosR)) * limb;
}

// Estrelas (só à noite): uma por célula de uma grade em volta da câmera, com magnitude sorteada (muitas fracas, poucas
// fortes), desenhadas como uma gaussiana do tamanho de ~1 pixel que integra a iluminância da estrela — o brilho no
// pixel não depende da resolução. Apagadas perto do horizonte pela transmitância da atmosfera.
vec3 starField(vec3 dir) {
  if (frame.night.y <= 0.0 || dir.y <= 0.0) return vec3(0.0);
  const float GRID = 420.0;
  vec3 p = dir * GRID;
  vec3 cell = floor(p);
  uvec3 h = pcg3d(uvec3(ivec3(cell) + ivec3(4096)));
  vec3 r = vec3(h) * (1.0 / 4294967296.0);
  if (r.x > 0.012) return vec3(0.0);  // ~1 célula em 80 tem estrela
  vec3 jitter = vec3(pcg3d(h ^ uvec3(0x68e31da4u))) * (1.0 / 4294967296.0);
  vec3 star = normalize(cell + 0.2 + 0.6 * jitter);  // longe da borda da célula
  float pixelAngle = 2.0 * frame.renderSize.w / abs(frame.proj[1][1]);  // ângulo de um pixel (proj[1][1] = ±1/tan(fov/2))
  float sigma = 0.6 * pixelAngle;
  float theta = acos(clamp(dot(dir, star), -1.0, 1.0));
  float magnitude = -1.0 + 7.0 * sqrt(r.y);                    // de -1 (Sírius) a 6 (limite do olho)
  float E = frame.night.y * pow(10.0, -0.4 * magnitude);       // lux
  vec3 tint = mix(vec3(0.85, 0.9, 1.0), vec3(1.0, 0.92, 0.8), r.z * 80.0);  // de levemente azuladas a levemente amareladas
  float profile = exp(-0.5 * theta * theta / (sigma * sigma)) / (TWO_PI * sigma * sigma);  // 1/sr, integra 1
  return E * profile * tint * preExposure() * transmittanceToSun(planetPos(frame.cameraPos.xyz), dir);
}

// Perspectiva aérea: atmosfera entre a câmera e um ponto da cena (volume 32³, até 32 km).
vec3 applyAerialPerspective(vec3 color, vec2 uv, vec3 worldPos) {
  float distKm = length(worldPos - frame.cameraPos.xyz) * 0.001;
  float w = sqrt(clamp(distKm / 32.0, 0.0, 1.0));  // inverso da distribuição quadrática das fatias
  // A fatia s guarda o acumulado até w = (s + 1) / N; antes da 1ª fatia, interpola desde "sem atmosfera".
  vec3 size = vec3(textureSize(aerialPerspectiveLut, 0));
  float slice = w * size.z - 1.0;
  vec4 ap = textureLod(aerialPerspectiveLut, vec3(uv, (max(slice, 0.0) + 0.5) / size.z), 0.0);
  float fade = clamp(slice + 1.0, 0.0, 1.0);
  ap = mix(vec4(0.0, 0.0, 0.0, 1.0), ap, fade);
  return color * ap.a + ap.rgb;
}

// Névoa exponencial por altura com in-scattering do sol e do céu.
vec3 applyHeightFog(vec3 color, vec3 worldPos) {
  float density = frame.params.z, falloff = frame.params.w;
  vec3 ro = frame.cameraPos.xyz;
  vec3 d = worldPos - ro;
  float dist = length(d);
  vec3 v = d / max(dist, 1e-4);
  float k = falloff * d.y;
  float lineIntegral = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
  float T = exp(-density * exp(-falloff * ro.y) * lineIntegral * dist);
  vec3 sunLight = sunIrradianceAt(ro) * henyeyGreenstein(dot(v, frame.sunDir.xyz), 0.65);
  vec3 ambient = sampleSky(normalize(vec3(v.x, 0.15, v.z)));
  return color * T + (sunLight + ambient) * (1.0 - T);
}

#endif
