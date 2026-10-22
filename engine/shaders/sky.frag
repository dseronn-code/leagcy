#version 460
#extension GL_GOOGLE_include_directive : require
#include "frame.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
  vec4 p = frame.invViewProj * vec4(vUV * 2.0 - 1.0, 0.5, 1.0);
  vec3 dir = normalize(p.xyz / p.w - frame.cameraPos.xyz);
  vec3 color = sampleSky(dir);

  // Disco solar com escurecimento de borda (limb darkening).
  float cosR = cos(frame.params.x);
  float mu = dot(dir, frame.sunDir.xyz);
  if (mu > cosR) {
    float r = sqrt(max(0.0, 1.0 - mu * mu)) / sin(frame.params.x);
    float limb = 1.0 - 0.6 * (1.0 - sqrt(max(0.0, 1.0 - r * r)));
    float solidAngle = TWO_PI * (1.0 - cosR);
    color += sunIrradianceAt(frame.cameraPos.xyz) / solidAngle * limb;
  }
  // O céu fica "atrás" da névoa: trata como superfície distante.
  color = applyHeightFog(color, frame.cameraPos.xyz + dir * 4000.0);
  outColor = vec4(color, 1.0);
}
