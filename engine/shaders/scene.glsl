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

struct Material {  // == scene::Material
  vec4 baseColor;
  vec3 emissive;
  float alphaCutoff;
  float metallic;
  float roughness;
  float normalScale;
  uint flags;
  int baseColorTex;
  int metalRoughTex;
  int normalTex;
  int emissiveTex;
};

struct InstanceData {  // == InstanceGpu em scene.cpp
  uint64_t vertices;
  uint64_t indices;
  uint material;
  uint pad0;
  uint64_t pad1;
};

struct PointLight {  // == scene::PointLight
  vec3 position;
  float radius;
  vec3 intensity;
  float range;
};

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

// Janela suave que leva a influência da luz a zero no alcance (evita corte seco).
float lightWindow(PointLight l, float dist) {
  float x = dist / l.range;
  float w = clamp(1.0 - x * x * x * x, 0.0, 1.0);
  return w * w;
}

// Iluminância aproximada (lux) a partir do centro da luz: usada só como função-alvo do ReSTIR.
vec3 lightIlluminanceApprox(PointLight l, float dist) {
  float d = max(dist, l.radius);
  return l.intensity * lightWindow(l, dist) / (d * d);
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
    E = l.intensity * lightWindow(l, d) / (r * r);
    return true;
  }
  float sinMax2 = r * r / d2;
  float cosMax = sqrt(max(0.0, 1.0 - sinMax2));
  float oneMinusCos = sinMax2 / (1.0 + cosMax);  // evita cancelamento numérico em cones minúsculos
  L = sampleCone(toC / d, cosMax, u);
  float b = dot(toC, L);
  tSurface = b - sqrt(max(b * b - (d2 - r * r), 0.0));
  float luminanceOfSurface = 1.0 / (PI * r * r);  // × intensidade: L = I / (π r²)
  E = l.intensity * lightWindow(l, d) * luminanceOfSurface * (TWO_PI * oneMinusCos);
  return true;
}

#endif
