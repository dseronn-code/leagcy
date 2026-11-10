#include "fcsim/ball.h"

#include <algorithm>
#include <cstring>

namespace fcsim {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr Vec3 kUp{0.0f, 1.0f, 0.0f};
constexpr float kG = 9.81f;

float dragCoefficient(const BallParams& p, float speed) {
  if (speed <= p.crisisFrom) return p.cdLow;
  if (speed >= p.crisisTo) return p.cdHigh;
  float t = (speed - p.crisisFrom) / (p.crisisTo - p.crisisFrom);
  t = t * t * (3.0f - 2.0f * t);
  return p.cdLow + (p.cdHigh - p.cdLow) * t;
}

// Momento de inércia de uma casca esférica: I = 2/3 m r².
float inertia(const BallParams& p) { return (2.0f / 3.0f) * p.mass * p.radius * p.radius; }

// Velocidade do ponto de contato com o chão (embaixo da bola).
Vec3 contactVelocity(const BallState& b, const BallParams& p) { return b.vel + cross(b.spin, Vec3{0.0f, -p.radius, 0.0f}); }

// Impulso tangencial J (N·s) aplicado no ponto de contato, contra o deslizamento: muda velocidade e spin.
void applyContactImpulse(BallState& b, const BallParams& p, const Vec3& J) {
  b.vel += J / p.mass;
  b.spin += cross(Vec3{0.0f, -p.radius, 0.0f}, J) / inertia(p);
}

// Impulso que zera o deslizamento do ponto de contato (massa efetiva de uma casca esférica: m·I/(I + m r²) = 2/5 m).
Vec3 impulseToStopSlip(const BallParams& p, const Vec3& slip) { return slip * (-0.4f * p.mass); }

}  // namespace

int substepBall(BallState& b, const BallParams& p) {
  const float dt = kBallSubstep;
  int bounces = 0;
  const float area = kPi * p.radius * p.radius;
  const float speed = length(b.vel);

  if (!b.onGround) {
    // ---- voo ----
    Vec3 acc{0.0f, -kG, 0.0f};
    if (speed > 1e-4f) {
      const float k = 0.5f * p.airDensity * area / p.mass;
      acc += b.vel * (-k * dragCoefficient(p, speed) * speed);
      const float w = length(b.spin);
      if (w > 1e-3f) {
        const float S = p.radius * w / speed;
        const float cl = std::min(p.liftMax, p.liftPerSpin * S);
        const Vec3 dir = normalizeOr(cross(b.spin / w, b.vel / speed), Vec3{});
        acc += dir * (k * cl * speed * speed);
      }
    }
    b.vel += acc * dt;  // Euler semi-implícito: velocidade antes da posição
    b.pos += b.vel * dt;
    b.spin *= 1.0f - dt / p.spinTau;
    if (b.pos.y <= p.radius && b.vel.y < 0.0f) {
      // ---- quique ----
      b.pos.y = p.radius;
      const float vn = -b.vel.y;
      if (vn < p.settleSpeed) {
        b.vel.y = 0.0f;
        b.onGround = true;
      } else {
        b.vel.y = vn * p.restitution;
        ++bounces;
        // atrito no contato durante o quique (Coulomb, limitado pelo impulso normal)
        const Vec3 slip = contactVelocity(b, p) - kUp * dot(contactVelocity(b, p), kUp);
        Vec3 J = impulseToStopSlip(p, slip);
        const float maxJ = p.bounceFriction * p.mass * vn * (1.0f + p.restitution);
        const float jl = length(J);
        if (jl > maxJ) J = J * (maxJ / jl);
        applyContactImpulse(b, p, J);
      }
    }
    return bounces;
  }

  // ---- no chão: desliza até rolar, depois rola perdendo velocidade ----
  b.pos.y = p.radius;
  b.vel.y = 0.0f;
  const Vec3 slip = contactVelocity(b, p);
  const float slipSpeed = length(slip);
  if (slipSpeed > 0.02f) {
    Vec3 J = impulseToStopSlip(p, slip);
    const float maxJ = p.slideFriction * p.mass * kG * dt;
    const float jl = length(J);
    if (jl > maxJ) J = J * (maxJ / jl);
    applyContactImpulse(b, p, J);
  } else {
    const float v = length(b.vel);
    if (v > 1e-4f) {
      const float nv = std::max(0.0f, v - p.rollDecel * dt);
      b.vel *= nv / v;
    }
    // rolamento puro: spin horizontal casado com a velocidade; o spin vertical (pião) some devagar
    const float spinY = b.spin.y * (1.0f - dt / 2.0f);
    b.spin = cross(kUp, b.vel) / p.radius;
    b.spin.y = spinY;
  }
  // arrasto do ar também no chão
  if (speed > 1e-4f) {
    const float k = 0.5f * p.airDensity * area / p.mass;
    const Vec3 v = b.vel;
    b.vel += v * (-k * dragCoefficient(p, speed) * speed * dt);
  }
  b.pos += b.vel * dt;
  if (b.vel.y > 0.0f) b.onGround = false;
  return bounces;
}

int stepBall(BallState& b, const BallParams& p, float dt) {
  const int n = int(dt / kBallSubstep + 0.5f);
  int bounces = 0;
  for (int i = 0; i < n; ++i) bounces += substepBall(b, p);
  return bounces;
}

uint64_t hashBall(const BallState& b) {
  // FNV-1a sobre os bits exatos.
  float f[9] = {b.pos.x, b.pos.y, b.pos.z, b.vel.x, b.vel.y, b.vel.z, b.spin.x, b.spin.y, b.spin.z};
  uint64_t h = 1469598103934665603ull;
  unsigned char bytes[sizeof(f)];
  std::memcpy(bytes, f, sizeof(f));
  for (unsigned char c : bytes) { h ^= c; h *= 1099511628211ull; }
  h ^= b.onGround ? 1u : 0u;
  return h;
}

}  // namespace fcsim
