// Partículas sem estado: posição, idade, tamanho e cor de cada uma saem de uma fórmula do tempo e de um hash do
// índice (o ciclo de vida se repete com fase própria). Sem simulação nem buffer para guardar — determinístico (testes,
// TAA) e um único desenho instanciado para milhares delas. Emissores em scene::ParticleEmitter (espelhado aqui).
#ifndef PARTICLES_GLSL
#define PARTICLES_GLSL

#include "frame.glsl"

const uint PARTICLE_FIRE = 0u, PARTICLE_SMOKE = 1u, PARTICLE_SPARKS = 2u, PARTICLE_RAIN = 3u, PARTICLE_SPLASH = 4u;

struct ParticleEmitter {  // == scene::ParticleEmitter
  vec3 position;
  uint type;
  float radius;  // raio da base do emissor (m)
  float scale;   // altura/tamanho típico (m)
  uint first, count;  // faixa de índices de partículas
};
layout(set = 1, binding = 0, std430) readonly buffer Emitters { ParticleEmitter emitters[]; };

vec3 hash3(uint i, uint salt) { return vec3(pcg3d(uvec3(i, salt, 0x51ed27u))) * (1.0 / 4294967296.0); }

// Cor de corpo negro aproximada (Tanner Helland), normalizada em luminância 1: chama de 1000 K (vermelha) a 1900 K.
vec3 blackbody(float kelvin) {
  float t = kelvin / 100.0;
  vec3 c;
  c.r = 1.0;
  c.g = clamp(0.39008158 * log(t) - 0.63184144, 0.0, 1.0);
  c.b = t <= 19.0 ? 0.0 : clamp(0.54320678 * log(t - 10.0) - 1.19625408, 0.0, 1.0);
  return c / max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
}

struct ParticleState {
  vec3 position, velocity;
  float size, alpha;
  vec3 emission;  // nits (pré-exposição aplicada depois)
  vec3 albedo;    // espalhamento (fumaça), iluminada por sol e céu
  bool streak;    // desenha alongada na direção do movimento (chuva, faíscas)
};

ParticleState particleState(ParticleEmitter e, uint i, float time) {
  ParticleState p;
  vec3 h = hash3(i, e.type * 7919u + 13u), g = hash3(i, 0x3c6ef372u);
  p.streak = false;
  p.emission = vec3(0.0);
  p.albedo = vec3(0.0);
  if (e.type == PARTICLE_RAIN) {
    // Chuva: caixa de 40 m em volta da câmera, pingos a 9 m/s, posição que "dá a volta" na caixa.
    const float BOX = 40.0, SPEED = 9.0;
    vec3 base = h * BOX;
    vec3 v = vec3(0.6, -SPEED, 0.25);
    vec3 q = base + v * time;
    vec3 rel = mod(q - frame.cameraPos.xyz + BOX * 0.5, BOX) - BOX * 0.5;
    p.position = frame.cameraPos.xyz + rel;
    p.velocity = v;
    p.size = 0.012;
    p.alpha = 0.22 * e.scale * rainExposure(p.position);  // scale = intensidade da chuva; sob telhado não cai
    p.albedo = vec3(1.0);
    p.streak = true;
    return p;
  }
  if (e.type == PARTICLE_SPLASH) {
    // Respingo do pingo no chão: nasce na superfície exposta ao céu (mapa de oclusão da chuva), abre e some em 0,25 s.
    // Sem o mapa (sem ray tracing) não há como achar o chão: nenhum respingo.
    const float LIFE = 0.25, AREA = 24.0;
    float cycle = time / LIFE + g.x;
    uvec3 hh = pcg3d(uvec3(i, uint(floor(cycle)), 0x7a3bu));  // posição nova a cada ciclo
    vec2 r = vec2(hh.xy) * (1.0 / 4294967296.0);
    vec2 xz = frame.cameraPos.xz + (r - 0.5) * AREA;
    vec2 uv = (xz - rainMapCenter()) / RAIN_EXTENT + 0.5;
    float ground = frame.weather.z < 0.0 ? -1e4 : textureLod(rainOcclusionMap, uv, 0.0).r;
    float age01 = fract(cycle);
    p.position = vec3(xz.x, ground + 0.02 + 0.05 * age01, xz.y);
    p.velocity = vec3(0.0);
    p.size = 0.015 + 0.06 * age01;
    p.alpha = ground > -1e3 ? 0.35 * e.scale * (1.0 - age01) : 0.0;
    p.albedo = vec3(1.0);
    return p;
  }
  float life = e.type == PARTICLE_SMOKE ? 6.0 : e.type == PARTICLE_SPARKS ? 1.4 : 1.1;
  float age01 = fract(time / life + g.x);  // 0 = nasce, 1 = morre
  float age = age01 * life;
  float ang = 6.2831853 * h.x, r = e.radius * sqrt(h.y);
  vec3 origin = e.position + vec3(cos(ang) * r, 0.0, sin(ang) * r);
  if (e.type == PARTICLE_FIRE) {
    // Chama: sobe por empuxo acelerando, afina e esfria (de ~1900 K amarela-branca a ~1100 K vermelha).
    vec3 v = vec3((g.y - 0.5) * 0.3, 1.2 + 1.5 * g.z, (h.z - 0.5) * 0.3);
    p.position = origin + v * age + vec3(0.0, 1.6 * age * age, 0.0) + 0.15 * sin(vec3(3.1, 0.0, 2.7) * age + 6.0 * h);
    p.velocity = v + vec3(0.0, 3.2 * age, 0.0);
    p.size = e.scale * 0.35 * (1.0 - 0.6 * age01) * (0.7 + 0.6 * h.z);
    float kelvin = mix(1900.0, 1100.0, age01);
    // Chama: volume opticamente fino, a luminância é a soma das partículas na visada (~40 se sobrepõem no meio da
    // chama): cada uma leva ~1/40 dos ~25 mil nits de uma fogueira.
    p.emission = blackbody(kelvin) * 600.0 * pow(1.0 - age01, 1.5);
    p.alpha = 0.0;  // luz pura (somada)
  } else if (e.type == PARTICLE_SMOKE) {
    // Fumaça: sobe devagar, abre e espalha com o vento, clareia e some.
    vec3 v = vec3(0.35 + 0.2 * g.y, 0.9 + 0.4 * g.z, 0.15 * (h.z - 0.5));
    p.position = origin + vec3(0.0, e.scale * 0.6, 0.0) + v * age + 0.3 * sin(vec3(0.7, 0.0, 0.9) * age + 6.0 * h);
    p.velocity = v;
    p.size = e.scale * (0.4 + 0.5 * age);
    p.alpha = 0.12 * smoothstep(0.0, 0.1, age01) * (1.0 - age01);  // muitas se sobrepõem: cada uma é tênue
    p.albedo = vec3(mix(0.3, 0.7, age01));  // fumaça de lenha: cinza que clareia ao se diluir
  } else {  // faíscas: saem rápido, caem por gravidade, apagam
    vec3 dir = normalize(vec3(h.x - 0.5, 0.6 + 0.8 * h.y, h.z - 0.5));
    vec3 v0 = dir * (3.0 + 4.0 * g.y) * e.scale;
    p.position = origin + vec3(0.0, e.scale * 0.3, 0.0) + v0 * age + vec3(0.0, -4.9, 0.0) * age * age;
    p.velocity = v0 + vec3(0.0, -9.8 * age, 0.0);
    p.size = 0.015;
    p.emission = blackbody(mix(2200.0, 1300.0, age01)) * 10000.0 * (1.0 - age01);  // metal incandescente
    p.alpha = 0.0;
    p.streak = true;
  }
  return p;
}

#endif
