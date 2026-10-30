// Dados da cena (set 0, bindings 2-5 e 7): materiais, instâncias, luzes, texturas bindless e objetos.
#ifndef SCENE_GLSL
#define SCENE_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

#include "common.glsl"

const uint MATERIAL_ALPHA_MASK = 1u;
const uint MATERIAL_TERRAIN = 2u;
const uint MATERIAL_SPECULAR_GLOSSINESS = 4u;  // KHR_materials_pbrSpecularGlossiness
const uint MATERIAL_EMISSION_SAMPLED = 8u;  // emissão já contada pela amostragem de luzes (scene.h)
const uint MATERIAL_LIGHT_GEOMETRY = 16u;   // forma visível de uma luz analítica: sem sombra
const uint MATERIAL_DOUBLE_SIDED = 32u;
const uint MATERIAL_TRANSMISSIVE = 64u;     // vidro: sem sombra (por enquanto)
const uint MATERIAL_CLEARCOAT = 128u;       // verniz (KHR_materials_clearcoat)
const uint MATERIAL_WATER = 256u;           // mar (renderer/ocean.h)
const uint MATERIAL_FOLIAGE = 512u;         // balança com o vento (gbuffer.vert)

// Simulação do oceano (renderer/ocean.cpp): repete a cada OCEAN_PATCH metros.
layout(set = 0, binding = 17) uniform sampler2D oceanDisplacement;  // xyz = deslocamento, w = Jacobiano
layout(set = 0, binding = 18) uniform sampler2D oceanSlope;         // xy = inclinação, z = espuma
const float OCEAN_PATCH = 256.0;

struct Material {  // == scene::Material (128 bytes)
  vec4 baseColor;      // metálico/rugosidade: cor base | especular/glossiness: cor difusa
  vec3 emissive;
  float alphaCutoff;
  float metallic;
  float roughness;     // especular/glossiness: 1 - glossiness
  float normalScale;
  uint flags;
  int baseColorTex;
  int metalRoughTex;   // especular/glossiness: textura RGB especular + A glossiness
  int normalTex;
  int emissiveTex;
  vec3 specularColor;  // KHR_materials_specular | especular/glossiness: cor especular
  float specular;      // KHR_materials_specular: peso
  float ior;           // KHR_materials_ior
  int specularTex;     // A = peso especular
  int specularColorTex;
  int occlusionTex;  // R: oclusão do glTF (só luz indireta)
  float transmission;  // KHR_materials_transmission (parede fina)
  int transmissionTex;
  float clearcoat;     // KHR_materials_clearcoat
  float clearcoatRoughness;
  int clearcoatTex;
  int clearcoatRoughnessTex;
  float flakes;      // flocos metálicos (pintura de carro)
  float flakeScale;  // células por unidade de UV
};

struct InstanceData {  // == InstanceGpu em scene.cpp
  uint64_t vertices;
  uint64_t indices;
  uint material;
  uint pad0;
  uint64_t pad1;
};

struct PointLight {  // == scene::PointLight (esfera/spot ou triângulo emissivo)
  vec3 position;   // triângulo: vértice 0
  float radius;
  vec3 intensity;  // triângulo: radiância média (só para escolher luzes; a exata vem do material)
  float range;
  vec3 direction;  // spot: eixo | triângulo: aresta 1
  float spotScale;  // pontual: escala 0, deslocamento 1
  vec3 edge2;      // triângulo: aresta 2
  float spotOffset;
  uint type;
  uint object, primitive;
  float area;
};
const uint LIGHT_SPHERE = 0u, LIGHT_TRIANGLE = 1u;

layout(set = 0, binding = 2, std430) readonly buffer Materials { Material materials[]; };
layout(set = 0, binding = 3, std430) readonly buffer Instances { InstanceData instances[]; };
layout(set = 0, binding = 4, std430) readonly buffer Lights { PointLight lights[]; };
layout(set = 0, binding = 5) uniform sampler2D textures[];

struct ObjectData {  // == ObjectGpu em renderer.cpp
  mat4 model;
  mat4 prevModel;
};
layout(set = 0, binding = 7, std430) readonly buffer Objects { ObjectData objects[]; };

// Vértice: posição(3) normal(3) uv(2) tangente(4) = 12 floats.
layout(buffer_reference, scalar) readonly buffer VertexData { float v[]; };
layout(buffer_reference, scalar) readonly buffer IndexData { uint i[]; };

vec3 fetchVertexPosition(VertexData vb, uint i) { return vec3(vb.v[i * 12u], vb.v[i * 12u + 1u], vb.v[i * 12u + 2u]); }
vec3 fetchVertexNormal(VertexData vb, uint i) { return vec3(vb.v[i * 12u + 3u], vb.v[i * 12u + 4u], vb.v[i * 12u + 5u]); }
vec2 fetchVertexUV(VertexData vb, uint i) { return vec2(vb.v[i * 12u + 6u], vb.v[i * 12u + 7u]); }

// Janela suave que leva a influência da luz a zero no alcance (evita corte seco), vezes o cone do spot
// (KHR_lights_punctual), avaliado na direção do centro da luz até P.
float lightWindow(PointLight l, vec3 P, float dist) {
  if (l.type == LIGHT_TRIANGLE) return 1.0;  // luz de malha: física, sem alcance artístico
  float x = dist / l.range;
  float w = clamp(1.0 - x * x * x * x, 0.0, 1.0);
  float spot = 1.0;
  if (l.type == LIGHT_SPHERE && l.spotScale > 0.0) {
    float cd = dist > 0.0 ? dot(l.direction, (P - l.position) / dist) : 1.0;
    spot = clamp(cd * l.spotScale + l.spotOffset, 0.0, 1.0);
    spot *= spot;
  }
  return w * w * spot;
}

// Iluminância aproximada (lux) a partir do centro da luz: usada só como função-alvo do ReSTIR.
vec3 lightIlluminanceApprox(PointLight l, vec3 P, float dist) {
  float d = max(dist, l.radius);
  return l.intensity * lightWindow(l, P, dist) / (d * d);
}

// Amostra uma direção uniforme no cone subtendido por uma luz esférica (pdf = 1/ângulo sólido).
// Devolve a direção, a distância até a superfície da esfera e a iluminância equivalente
// E = luminância × ângulo sólido; o estimador é BRDF × cos × E. Para raio → 0, E → I / d².
bool sampleSphereLight(PointLight l, vec3 P, vec2 u, out vec3 L, out float tSurface, out vec3 E) {
  vec3 toC = l.position - P;
  float d2 = dot(toC, toC);
  float d = sqrt(d2);
  float r = max(l.radius, 1e-3);
  if (d <= r) {  // ponto dentro da esfera: ilumina por igual, sem sombra
    L = d > 0.0 ? toC / d : vec3(0, 1, 0);
    tSurface = 0.0;
    E = l.intensity * lightWindow(l, P, d) / (r * r);
    return true;
  }
  float sinMax2 = r * r / d2;
  float cosMax = sqrt(max(0.0, 1.0 - sinMax2));
  float oneMinusCos = sinMax2 / (1.0 + cosMax);  // evita cancelamento numérico em cones minúsculos
  L = sampleCone(toC / d, cosMax, u);
  float b = dot(toC, L);
  tSurface = b - sqrt(max(b * b - (d2 - r * r), 0.0));
  float luminanceOfSurface = 1.0 / (PI * r * r);  // × intensidade: L = I / (π r²)
  E = l.intensity * lightWindow(l, P, d) * luminanceOfSurface * (TWO_PI * oneMinusCos);
  return true;
}

// Ponto uniforme num triângulo a partir de u em [0,1)² (coordenadas baricêntricas b1, b2).
vec2 triangleBarycentric(vec2 u) {
  float su = sqrt(u.x);
  return vec2(1.0 - su, u.y * su);
}

// Radiância emitida (nits) pelo triângulo de uma luz de malha nas baricêntricas b: fator emissivo do material × textura
// emissiva nas UVs interpoladas (o mesmo que a câmera vê).
vec3 triangleLightRadiance(PointLight l, vec2 b, out bool doubleSided) {
  InstanceData d = instances[l.object];
  Material m = materials[d.material];
  doubleSided = (m.flags & MATERIAL_DOUBLE_SIDED) != 0u;
  vec3 Le = m.emissive;
  if (m.emissiveTex >= 0) {
    IndexData ib = IndexData(d.indices);
    VertexData vb = VertexData(d.vertices);
    uint i0 = ib.i[3u * l.primitive], i1 = ib.i[3u * l.primitive + 1u], i2 = ib.i[3u * l.primitive + 2u];
    vec2 uv = fetchVertexUV(vb, i0) * (1.0 - b.x - b.y) + fetchVertexUV(vb, i1) * b.x + fetchVertexUV(vb, i2) * b.y;
    Le *= textureLod(textures[nonuniformEXT(m.emissiveTex)], uv, 0.0).rgb;
  }
  return Le;
}

#endif
