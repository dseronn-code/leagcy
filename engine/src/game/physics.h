#pragma once
#include <cstdint>
#include <memory>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace game {

// Como um corpo se move: parado (chão, paredes), pela simulação (caixas, destroços) ou só pelo jogo/animação
// (plataformas, portas: empurra os outros sem ser empurrado).
enum class Motion { Static, Dynamic, Kinematic };

struct BodyDesc {
  Motion motion = Motion::Static;
  float mass = 0.0f;         // kg; 0 = calculada pela forma com densidade de 1.000 kg/m³
  float friction = 0.5f;
  float restitution = 0.1f;  // quique (0 = nenhum, 1 = elástico)
  float linearDamping = 0.05f, angularDamping = 0.05f;
};

// Física de corpos rígidos sobre o Jolt Physics: passos fixos de 1/60 s com 2 subpassos de colisão (resultado igual em
// qualquer taxa de quadros e determinístico entre execuções), em várias threads. Os cabeçalhos do Jolt ficam só em
// physics.cpp.
class Physics {
 public:
  explicit Physics(uint32_t maxBodies = 65536);
  ~Physics();
  Physics(const Physics&) = delete;
  Physics& operator=(const Physics&) = delete;

  // Formas básicas; devolvem o identificador do corpo. O corpo entra ativo (dinâmicos começam a cair).
  uint32_t addBox(glm::vec3 halfExtents, glm::vec3 position, glm::quat rotation, const BodyDesc& desc);
  uint32_t addSphere(float radius, glm::vec3 position, glm::quat rotation, const BodyDesc& desc);
  uint32_t addCapsule(float halfHeight, float radius, glm::vec3 position, glm::quat rotation, const BodyDesc& desc);

  // Avança o tempo real `dt`: roda os passos fixos que couberem e guarda o resto para o próximo quadro.
  // Prepare os corpos (optimizeBroadphase) depois de criar muitos de uma vez.
  void update(float dt);
  void optimizeBroadphase();
  // Fração do próximo passo fixo já decorrida (0..1): interpolar entre a pose anterior e a atual.
  float interpolation() const;

  void pose(uint32_t body, glm::vec3& position, glm::quat& rotation) const;
  glm::vec3 linearVelocity(uint32_t body) const;
  void setLinearVelocity(uint32_t body, glm::vec3 v);
  void addImpulse(uint32_t body, glm::vec3 impulse);
  // Corpo cinemático: chega em `position`/`rotation` ao fim do próximo passo (empurra os dinâmicos no caminho).
  void moveKinematic(uint32_t body, glm::vec3 position, glm::quat rotation);
  bool isActive(uint32_t body) const;

  uint32_t bodyCount() const;
  uint32_t activeBodyCount() const;
  uint64_t steps() const;  // passos fixos já simulados
  static constexpr float kStep = 1.0f / 60.0f;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
