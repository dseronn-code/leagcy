#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"
#include "scene.glsl"

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aTangent;
layout(location = 4) in vec3 aPrevPosition;  // posição do frame anterior (malhas deformáveis)

// Desenho instanciado: uma chamada por malha; a instância (gl_InstanceIndex, já somado ao firstInstance do lote)
// indexa a lista de objetos ordenada por malha.
layout(buffer_reference, scalar) readonly buffer DrawList { uint objects[]; };
layout(push_constant) uniform Push { uint64_t drawList; } pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;
layout(location = 3) out vec4 vTangent;
layout(location = 4) out vec4 vCurClip;   // sem jitter, para os vetores de movimento
layout(location = 5) out vec4 vPrevClip;
layout(location = 6) flat out uint vObject;
layout(location = 7) flat out uint vMaterial;

void main() {
  vObject = DrawList(pc.drawList).objects[gl_InstanceIndex];
  vMaterial = instances[vObject].material;
  ObjectData o = objects[vObject];
  if ((materials[vMaterial].flags & MATERIAL_WATER) != 0u) {
    // Mar: a malha (radial, em volta da origem) segue a câmera em passos de 2 m e é deslocada pelas ondas, lidas na
    // posição do mundo (a forma não "nada" com a câmera). As ondas somem ao longe (já menores que um pixel).
    vec2 snapped = floor(frame.cameraPos.xz * 0.5) * 2.0;
    vec3 base = vec3(snapped.x + aPosition.x, o.model[3].y, snapped.y + aPosition.z);
    float fade = 1.0 - smoothstep(800.0, 4000.0, distance(base.xz, frame.cameraPos.xz));
    vec3 world = base + textureLod(oceanDisplacement, base.xz / OCEAN_PATCH, 0.0).xyz * fade;
    vWorldPos = world;
    vNormal = vec3(0.0, 1.0, 0.0);
    vTangent = vec4(1.0, 0.0, 0.0, 1.0);
    vUV = world.xz;
    vCurClip = frame.viewProjNoJitter * vec4(world, 1.0);
    vPrevClip = frame.prevViewProjNoJitter * vec4(world, 1.0);
    gl_Position = frame.proj * vec4(mat3(frame.view) * (world - frame.cameraPos.xyz), 1.0);
    return;
  }
  vec4 world = o.model * vec4(aPosition, 1.0);
  mat3 normalMatrix = transpose(inverse(mat3(o.model)));
  vWorldPos = world.xyz;
  vNormal = normalMatrix * aNormal;
  vTangent = vec4(mat3(o.model) * aTangent.xyz, aTangent.w);
  vUV = aUV;
  vCurClip = frame.viewProjNoJitter * world;
  vPrevClip = frame.prevViewProjNoJitter * (o.prevModel * vec4(aPrevPosition, 1.0));
  // Relativo à câmera (mundo aberto): a translação do objeto menos a da câmera é exata para valores próximos
  // (Sterbenz), então a projeção não multiplica coordenadas de 4 km. Mesma conta que viewProj * world.
  vec3 rel = mat3(o.model) * aPosition + (o.model[3].xyz - frame.cameraPos.xyz);
  gl_Position = frame.proj * vec4(mat3(frame.view) * rel, 1.0);
}
