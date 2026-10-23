// Dados da cena (set 0, bindings 2-5 e 7): materiais, instâncias, luzes, texturas bindless e objetos.
#ifndef SCENE_GLSL
#define SCENE_GLSL

#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

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

vec3 fetchVertexNormal(VertexData vb, uint i) { return vec3(vb.v[i * 12u + 3u], vb.v[i * 12u + 4u], vb.v[i * 12u + 5u]); }
vec2 fetchVertexUV(VertexData vb, uint i) { return vec2(vb.v[i * 12u + 6u], vb.v[i * 12u + 7u]); }

// Atenuação física (1/d²) com janela suave até o alcance da luz.
vec3 lightRadiance(PointLight l, float dist) {
  float x = dist / l.range;
  float window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
  // Distância mínima de 25 cm: evita a singularidade 1/d² quando uma luz encosta numa superfície.
  float d = max(dist, max(l.radius, 0.25));
  return l.intensity * window * window / (d * d);
}

#endif
