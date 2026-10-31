#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"
// Streaming de texturas: um pixel em 16 (posição que roda a cada frame) anota a resolução que a tela pede de cada
// textura (texels no maior eixo, pelo LOD que o hardware calcula, com o viés de mip). A CPU lê no frame seguinte.
layout(set = 0, binding = 24, std430) buffer TextureFeedback { uint textureNeed[]; };
vec4 sampleMaterialTexture(int i, vec2 uv) {
  const uint f = frame.frameInfo.x;
  if ((uint(gl_FragCoord.x) & 3u) == (f & 3u) && (uint(gl_FragCoord.y) & 3u) == ((f >> 2) & 3u)) {
    ivec2 size = textureSize(textures[nonuniformEXT(i)], 0);
    float lod = textureQueryLod(textures[nonuniformEXT(i)], uv).y + textureMipBias();
    atomicMax(textureNeed[i], uint(clamp(float(max(size.x, size.y)) * exp2(-lod), 1.0, 65536.0)));
  }
  return texture(textures[nonuniformEXT(i)], uv, textureMipBias());
}
#define MATERIAL_SAMPLE(i, uv) sampleMaterialTexture(i, uv)
#define MATERIAL_DERIVATIVES  // dFdx/dFdy disponíveis (flocos metálicos medem o tamanho do pixel)
#include "material.glsl"

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
layout(location = 3) in vec4 vTangent;
layout(location = 4) in vec4 vCurClip;
layout(location = 5) in vec4 vPrevClip;

layout(location = 0) out vec4 outDiffuse;   // rgb = cor difusa
layout(location = 1) out vec4 outNormal;    // xyz = normal (mundo), w = rugosidade (com AA especular)
layout(location = 2) out vec2 outMotion;    // deslocamento em UV desde o frame anterior
layout(location = 3) out vec4 outEmissive;
layout(location = 4) out vec4 outSpecular;  // rgb = f0, a = f90
layout(location = 5) out uvec2 outVisibility;  // objeto + 1, triângulo (superfície exata nos passes de RT)

layout(location = 6) flat in uint vObject;
layout(location = 7) flat in uint vMaterial;
layout(location = 8) flat in uint vLod;
layout(location = 9) in float vCoverage;
layout(location = 10) in vec4 vColor;
layout(location = 11) flat in uint vDither;

// Anti-aliasing especular geométrico (Tokuyoshi & Kaplanyan, "Improved Geometric Specular
// Antialiasing", I3D 2019): a variação da normal dentro do pixel alarga o lóbulo GGX, então
// superfícies curvas e normal maps finos não cintilam sob luz forte.
float filterRoughness(vec3 normal, float roughness, float normalVariance) {
  const float SIGMA2 = 0.15915494;  // variância do filtro de pixel (1 / 2π)
  const float KAPPA = 0.18;         // limite do alargamento
  vec3 dndu = dFdx(normal), dndv = dFdy(normal);
  float kernel = min(2.0 * SIGMA2 * (dot(dndu, dndu) + dot(dndv, dndv)), KAPPA);
  float a = roughness * roughness;
  float a2 = clamp(a * a + kernel + normalVariance, 0.0, 1.0);
  return sqrt(sqrt(a2));  // de volta para rugosidade perceptual
}

void main() {
  if ((vDither & 31u) != 0u) {
    // Troca suave de nível: o mesmo número por pixel para quem sai e quem entra (pontilhados complementares, nenhum
    // buraco nem pixel duplo), sorteado de novo a cada frame para o TAA/DLSS fazer a mistura.
    float u = float(pcg3d(uvec3(uvec2(gl_FragCoord.xy), frame.frameInfo.x)).x) * (1.0 / 4294967296.0);
    float t = float(vDither & 31u) / 32.0;
    if ((vDither & 32u) != 0u ? u >= t : u < t) discard;
  }
  Material m = materials[vMaterial];
  vec3 N = normalize(vNormal);
  if (!gl_FrontFacing) N = -N;  // materiais de dois lados
  Surface s = evalMaterial(m, vWorldPos, N, vTangent, vUV, vColor);
  // Misturados (decals BLEND): sem recorte, o alfa vai para a mistura (pipeline de decals: .a de cada anexo).
  bool blend = (m.flags & MATERIAL_BLEND) != 0u;
  // Cabelo mais fino que o pixel: transparência estocástica (o TAA/DLSS faz a média entre frames).
  if ((m.flags & MATERIAL_HAIR) != 0u &&
      // Sorteio independente por fio (índice do triângulo) e por frame: as camadas de fios se somam em vez de sumirem
      // juntas.
      float(pcg3d(uvec3(uvec2(gl_FragCoord.xy), uint(gl_PrimitiveID) * 64u + frame.frameInfo.x % 64u)).x) * (1.0 / 4294967296.0) > vCoverage)
    discard;
  if (!blend && (m.flags & MATERIAL_ALPHA_MASK) != 0u && s.alpha < m.alphaCutoff) discard;
  if (blend && s.alpha <= 0.002) discard;

  // Movimento da câmera + do objeto + da deformação do esqueleto.
  outMotion = (vCurClip.xy / vCurClip.w - vPrevClip.xy / vPrevClip.w) * 0.5;
  outDiffuse = vec4(s.diffuseColor, blend ? s.alpha : s.occlusion);  // a: oclusão do material (luz indireta)
  // Com verniz forte, a rugosidade do G-buffer é a do verniz (o lobo mais nítido decide o denoiser e as guias do
  // DLSS-RR); a iluminação relê a rugosidade da base no material.
  float gRoughness = s.clearcoat >= 0.5 ? filterRoughness(normalize(N), s.clearcoatRoughness, 0.0)
                                        : filterRoughness(s.normal, s.roughness, s.normalVariance);
  outNormal = vec4(s.normal, blend ? s.alpha : gRoughness);
  outEmissive = vec4(clampHalf(s.emissive * preExposure()), 1.0);  // nits, pré-exposto
  outSpecular = vec4(s.f0, blend ? s.alpha : s.f90);
  // Mar: a superfície deslocada não é o triângulo guardado (plano parado nos raios): os passes de RT reconstroem o
  // ponto pela profundidade (0 = sem triângulo exato).
  // O mesmo vale para os LODs (o triângulo é da malha simplificada, não da guardada no objeto).
  outVisibility = ((m.flags & MATERIAL_WATER) != 0u || vLod != 0u) ? uvec2(0u) : uvec2(vObject + 1u, uint(gl_PrimitiveID));
}
