#version 460
// Triângulo que cobre a tela inteira, em z = 0 (infinito no reverse-Z).
layout(location = 0) out vec2 vUV;
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  vUV = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
