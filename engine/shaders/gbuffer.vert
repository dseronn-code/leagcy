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
layout(location = 8) flat out uint vLod;
layout(location = 9) out float vCoverage;
layout(location = 10) out vec4 vColor;      // cor por vértice (COLOR_0), branco se não há
layout(location = 11) flat out uint vDither;  // troca suave de nível (cull.comp): fração em 1/32 | 32 = entrando  // cabelo: fração do pixel que o fio cobre (transparência estocástica)

// Vento na folhagem: rajadas que viajam pelo espaço (ondas lentas na direção do vento), curvatura que cresce com a
// altura acima da base do objeto (o tronco quase não mexe, a copa sim) e tremor rápido das folhas.
vec3 windSway(vec3 p, float height, float t) {
  const vec3 WIND = vec3(0.94, 0.0, 0.34);
  float gust = 0.6 + 0.4 * sin(t * 0.7 - dot(p.xz, WIND.xz) * 0.08);
  float sway = (sin(t * 1.4 - dot(p.xz, WIND.xz) * 0.25) * 0.7 + sin(t * 2.9 + p.x * 0.6 + p.z * 0.4) * 0.3) * gust;
  float bend = 0.02 * pow(min(height, 12.0), 1.4);
  float flutter = 0.015 * sin(t * 11.0 + dot(p, vec3(13.1, 7.7, 9.3)));
  return WIND * (bend * (0.6 + sway)) + vec3(flutter, flutter * 0.6, -flutter * 0.4) * min(height, 2.0);
}

void main() {
  uint entry = DrawList(pc.drawList).objects[gl_InstanceIndex];
  vObject = entry & 0xFFFFFFu;
  vDither = (entry >> 24) & 63u;
  vLod = entry >> 30;  // nível de detalhe (cull.comp)
  vMaterial = instances[vObject].material;
  vColor = fetchVertexColor(instances[vObject], uint(gl_VertexIndex));
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
  // Multidão: a pose deste frame e a do anterior vêm das poses assadas (vertex pulling pelo índice do vértice).
  vec3 position = aPosition, normal = aNormal, prevPosition = aPrevPosition;
  vec4 tangent = aTangent;
  if (o.anim.x != 0xFFFFFFFFu) {
    VertexData cur = VertexData(crowdFrames[o.anim.x + o.anim.y]), prv = VertexData(crowdFrames[o.anim.x + o.anim.z]);
    uint vi = uint(gl_VertexIndex);
    position = fetchVertexPosition(cur, vi);
    normal = fetchVertexNormal(cur, vi);
    tangent = vec4(cur.v[vi * 12u + 8u], cur.v[vi * 12u + 9u], cur.v[vi * 12u + 10u], cur.v[vi * 12u + 11u]);
    prevPosition = fetchVertexPosition(prv, vi);
  }
  vCoverage = 1.0;
  if ((materials[vMaterial].flags & MATERIAL_HAIR) != 0u) {
    // Fita do fio virada para a câmera: volta à linha central (a fita guardada é a do ray tracing) e abre na direção
    // perpendicular ao fio e à vista, com pelo menos meio pixel de largura; o que sobra vira cobertura < 1.
    float side = aUV.x * 2.0 - 1.0, w = aTangent.w;
    vec3 c = position - side * w * cross(normal, aTangent.xyz);  // linha central (lado = normal × tangente)
    vec3 cw = (o.model * vec4(c, 1.0)).xyz, tw = normalize(mat3(o.model) * aTangent.xyz);
    // Vento nos fios: a ponta balança mais que a raiz (posição ao longo do fio ao quadrado).
    float along = aUV.y * aUV.y;
    vec3 sway = windSway(cw, 6.0 * along, frame.night.w) * 0.08 * along;
    vec3 swayPrev = windSway(cw, 6.0 * along, frame.weather.w) * 0.08 * along;
    cw += sway;
    vec3 toCam = normalize(frame.cameraPos.xyz - cw);
    vec3 across = normalize(cross(tw, toCam) + vec3(1e-6));
    float dist = distance(frame.cameraPos.xyz, cw);
    float pixel = 2.0 * dist / (abs(frame.proj[1][1]) * frame.renderSize.y);  // tamanho de um pixel nessa distância
    float half_ = max(w * length(o.model[0].xyz), 0.5 * pixel);
    vCoverage = clamp(2.0 * w * length(o.model[0].xyz) / pixel, 0.0, 1.0);
    vec3 worldPos = cw + across * side * half_;
    vec3 facing = normalize(toCam - tw * dot(toCam, tw));
    vWorldPos = worldPos;
    vNormal = facing;
    vTangent = vec4(tw, 1.0);
    vUV = aUV;
    vCurClip = frame.viewProjNoJitter * vec4(worldPos, 1.0);
    vec3 cp = (o.prevModel * vec4(c, 1.0)).xyz + swayPrev;
    vPrevClip = frame.prevViewProjNoJitter * vec4(cp + across * side * half_, 1.0);
    gl_Position = frame.proj * vec4(mat3(frame.view) * (worldPos - frame.cameraPos.xyz), 1.0);
    return;
  }
  vec4 world = o.model * vec4(position, 1.0);
  vec4 prevWorld = o.prevModel * vec4(prevPosition, 1.0);
  vec3 wind = vec3(0.0);
  if ((materials[vMaterial].flags & MATERIAL_FOLIAGE) != 0u) {
    float height = max(world.y - o.model[3].y, 0.0);
    wind = windSway(world.xyz, height, frame.night.w);
    world.xyz += wind;
    prevWorld.xyz += windSway(prevWorld.xyz, height, frame.weather.w);  // tempo da cena no frame anterior
  }
  mat3 normalMatrix = transpose(inverse(mat3(o.model)));
  vWorldPos = world.xyz;
  vNormal = normalMatrix * normal;
  vTangent = vec4(mat3(o.model) * tangent.xyz, tangent.w);
  vUV = aUV;
  vCurClip = frame.viewProjNoJitter * world;
  vPrevClip = frame.prevViewProjNoJitter * prevWorld;
  // Relativo à câmera (mundo aberto): a translação do objeto menos a da câmera é exata para valores próximos
  // (Sterbenz), então a projeção não multiplica coordenadas de 4 km. Mesma conta que viewProj * world.
  vec3 rel = mat3(o.model) * position + (o.model[3].xyz - frame.cameraPos.xyz) + wind;
  gl_Position = frame.proj * vec4(mat3(frame.view) * rel, 1.0);
}
