// Corpo do vertex shader das partículas (particles.vert / particles_rt.vert).
// Quadrado de cada partícula, virado para a câmera (ou alongado na direção do movimento: chuva, faíscas). A luz da
// fumaça é calculada aqui uma vez por vértice: sol com sombra traçada até a partícula e céu.


#include "particles.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(push_constant) uniform Push { uint emitterCount; uint rayTracing; float time; uint pad; } pc;

layout(location = 0) out vec2 vCorner;
layout(location = 1) out vec4 vColor;      // rgb = luz (pré-exposta, já × alfa), a = alfa
layout(location = 2) out float vViewDepth;  // distância ao longo do eixo da câmera

void main() {
  uint i = gl_InstanceIndex;
  ParticleEmitter e = emitters[0];
  for (uint k = 0u; k < pc.emitterCount; ++k)
    if (i >= emitters[k].first && i < emitters[k].first + emitters[k].count) e = emitters[k];
  ParticleState p = particleState(e, i - e.first, pc.time);
  const vec2 corners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
  vec2 c = corners[gl_VertexIndex];
  vec3 toCam = normalize(frame.cameraPos.xyz - p.position);
  vec3 right, up;
  if (p.streak) {  // eixo maior na velocidade (o rastro de 1/60 s que o olho vê), largura de frente para a câmera
    up = p.velocity * (1.0 / 60.0) * 0.5;
    right = normalize(cross(up, toCam)) * p.size;
  } else {
    right = normalize(cross(vec3(0.0, 1.0, 0.0), toCam)) * p.size;
    up = normalize(cross(toCam, right)) * p.size;
  }
  vec3 world = p.position + right * c.x + up * c.y;
  vec3 light = p.emission;
  if (dot(p.albedo, p.albedo) > 0.0) {
    vec3 sunE = sunIrradianceAt(p.position) / max(preExposure(), 1e-20);
    float vis = 1.0;
#ifdef RAY_TRACING
    if (pc.rayTracing != 0u && dot(sunE, sunE) > 0.0) vis = traceVisibility(p.position, frame.sunDir.xyz, 1e4);
#endif
    vec3 sky = sampleSky(vec3(0.0, 1.0, 0.0)) / max(preExposure(), 1e-20);
    float phase = henyeyGreenstein(dot(-toCam, frame.sunDir.xyz), 0.3);
    light += p.albedo * (sunE * vis * phase + sky * 0.5);
  }
  float alpha = p.alpha;
  vColor = vec4(light * preExposure() * max(alpha, 1.0 - step(1e-6, alpha)), alpha);  // emissivo: soma pura
  if (e.type == PARTICLE_RAIN || e.type == PARTICLE_SPLASH)  // água: devolve a luz do céu (refração e reflexo)
    vColor.rgb = sampleSky(normalize(vec3(0.0, 1.0, 0.0) + toCam)) * 0.6 * alpha;
  vCorner = c;
  vec3 rel = world - frame.cameraPos.xyz;
  vViewDepth = -(mat3(frame.view) * rel).z;
  gl_Position = frame.proj * vec4(mat3(frame.view) * rel, 1.0);
}
