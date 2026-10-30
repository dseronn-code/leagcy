// Injeção da névoa: em cada célula, densidade e luz que chega (sol com sombra traçada, céu), com média temporal
// reprojetada. Saída: rgb = espalhamento (σs × radiância que chega, pré-exposta), a = extinção (1/m).
#extension GL_GOOGLE_include_directive : require
#include "fog.glsl"
#ifdef RAY_TRACING
#include "raytracing.glsl"
#endif

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 1, binding = 0, rgba16f) uniform writeonly image3D injectOut;
layout(set = 1, binding = 1) uniform sampler3D injectPrev;

void main() {
  ivec3 size = imageSize(injectOut);
  ivec2 g = ivec2(gl_GlobalInvocationID.xy);
  ivec3 id = ivec3(g.x, g.y % size.y, g.y / size.y);  // y e fatia no mesmo eixo do despacho
  if (g.x >= size.x || id.z >= size.z) return;
  vec2 uv = (vec2(id.xy) + 0.5) / vec2(size.xy);
  // Profundidade sorteada dentro da fatia a cada frame (a média temporal vira o integral da fatia).
  float jitter = fract(hash12(vec2(id.xy) + float(id.z) * 17.0) + float(frame.frameInfo.x) * 0.618034);
  float dist = fogSliceDistance((float(id.z) + jitter) / float(size.z));
  vec3 v = viewDirFromUv(uv);
  vec3 p = frame.cameraPos.xyz + v * dist;
  float sigmaT = fogDensityAt(p);
  vec3 S = vec3(0.0);
  if (sigmaT > 0.0) {
    vec3 sunE = sunIrradianceAt(p);
    float vis = 1.0;
#ifdef RAY_TRACING
    if (dot(sunE, sunE) > 0.0) vis = traceVisibility(p, frame.sunDir.xyz, 1e4);
#else
    if (shadowMapsEnabled()) vis = sunShadowMap(p, vec3(0.0));
#endif
    // Luz do céu: uma direção sorteada na esfera e um raio de visibilidade — dentro de um prédio o céu está tapado e a
    // névoa fica escura (estimador sem viés de ∫ céu × visibilidade × fase isotrópica; a média temporal tira o ruído).
    uvec3 h = pcg3d(uvec3(id.xy, uint(id.z) + frame.frameInfo.x * 64u));
    vec2 r = vec2(h.xy) * (1.0 / 4294967296.0);
    float z = 1.0 - 2.0 * r.x, sr = sqrt(max(1.0 - z * z, 0.0));
    vec3 w = vec3(sr * cos(TWO_PI * r.y), z, sr * sin(TWO_PI * r.y));
    vec3 ambient = sampleSky(w);
#ifdef RAY_TRACING
    ambient *= traceVisibility(p, w, 1e4);
#endif
    S = sigmaT * (sunE * vis * henyeyGreenstein(dot(v, frame.sunDir.xyz), FOG_PHASE_G) + ambient);
  }
  vec4 cur = vec4(S, sigmaT);
  // Histórico: a mesma posição do mundo na grade do frame anterior.
  vec4 prevClip = frame.prevViewProjNoJitter * vec4(p, 1.0);
  if (frame.frameInfo.w != 0u && prevClip.w > 0.0) {
    vec2 prevUv = prevClip.xy / prevClip.w * 0.5 + 0.5;
    float prevW = fogSliceCoord(distance(p, frame.prevCameraPos.xyz));
    vec3 c = vec3(prevUv, prevW);
    if (all(greaterThanEqual(c, vec3(0.0))) && all(lessThanEqual(c, vec3(1.0)))) cur = mix(textureLod(injectPrev, c, 0.0), cur, 0.1);
  }
  imageStore(injectOut, id, vec4(clampHalf(cur.rgb), cur.a));
}
