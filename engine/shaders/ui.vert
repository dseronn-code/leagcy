#version 460
// Camada 2D do jogo (HUD, botões, texto): triângulos em pixels da saída, desenhados depois do tonemap.
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;
layout(push_constant) uniform Push { vec2 size; uint srgbTarget; } pc;
layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUv;
void main() {
  vColor = inColor;
  vUv = inUv;
  gl_Position = vec4(inPos / pc.size * 2.0 - 1.0, 0.0, 1.0);  // Vulkan: y do clip para baixo = y do pixel
}
