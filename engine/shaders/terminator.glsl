// Correções do "shadow terminator" (onde luz e sombra se encontram numa superfície curva).
#ifndef TERMINATOR_GLSL
#define TERMINATOR_GLSL

// Hanika, "Hacking the Shadow Terminator" (Ray Tracing Gems II, cap. 4): normais suavizadas
// descrevem uma superfície curva, mas os raios saem da faceta plana, que fica ABAIXO dela — as facetas
// vizinhas fazem sombra em pontos que o sombreamento diz que estão na luz (degraus no terminator).
// Projeta o ponto nos planos tangentes dos três vértices e faz a média baricêntrica: o ponto sobe até
// a superfície curva aproximada. Em geometria plana (normais = faceta) não muda nada.
vec3 shadowTerminatorPoint(vec3 P, vec3 p0, vec3 p1, vec3 p2, vec3 n0, vec3 n1, vec3 n2, vec3 b) {
  vec3 d0 = P - p0, d1 = P - p1, d2 = P - p2;
  d0 -= min(dot(d0, n0), 0.0) * n0;
  d1 -= min(dot(d1, n1), 0.0) * n1;
  d2 -= min(dot(d2, n2), 0.0) * n2;
  return b.x * (p0 + d0) + b.y * (p1 + d1) + b.z * (p2 + d2);
}

// Chiang, Li & Burley, "Taming the Shadow Terminator" (SIGGRAPH 2019 Talks): o normal map inclina a
// normal para a luz onde a superfície real (Nb) já está quase de costas — o resultado é um corte seco
// no terminator. Este termo de sombreamento do relevo leva a luz a zero de forma suave.
float bumpShadowing(vec3 Nb, vec3 Ns, vec3 L) {
  float NbL = dot(Nb, L), NsL = dot(Ns, L);
  if (NbL <= 0.0 || NsL <= 0.0) return 0.0;
  float g = min(1.0, NbL / max(NsL * dot(Nb, Ns), 1e-6));
  return -g * g * g + g * g + g;
}

#endif
