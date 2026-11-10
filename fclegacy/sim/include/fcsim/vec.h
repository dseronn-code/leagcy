#pragma once
// Vetor 3D mínimo da simulação. Sem dependências: o mesmo código compila no celular, no servidor e nos testes.
#include <cmath>

namespace fcsim {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  constexpr Vec3() = default;
  constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
  constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
  constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
  constexpr Vec3 operator-() const { return {-x, -y, -z}; }
  constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
  constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
constexpr Vec3 operator*(float s, const Vec3& v) { return v * s; }
constexpr float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3& v) { return std::sqrt(dot(v, v)); }
inline Vec3 normalizeOr(const Vec3& v, const Vec3& fallback) {
  const float l = length(v);
  return l > 1e-6f ? v / l : fallback;
}

}  // namespace fcsim
