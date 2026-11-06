#pragma once
#include <cstdint>
#include <memory>
#include <vector>

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

// Personagem controlado pelo jogo (não pela simulação): anda, pula, sobe degraus e rampas até maxSlopeDeg, é
// empurrado por plataformas e empurra caixas. Cápsula com os pés na posição.
struct CharacterDesc {
  float height = 1.8f, radius = 0.3f;
  float mass = 70.0f;           // kg: força com que empurra corpos dinâmicos
  float maxSlopeDeg = 50.0f;    // rampa mais íngreme que ele sobe
  float stepHeight = 0.4f;      // degrau mais alto que ele sobe andando
};

// Carro de 4 rodas (Jolt WheeledVehicleController): suspensão, motor com curva de torque, câmbio automático,
// diferencial, direção nas dianteiras, freio de mão nas traseiras. Frente = +Z do carro, para cima = +Y.
struct VehicleDesc {
  float length = 4.2f, width = 1.8f, height = 0.9f;  // caixa do chassi (m)
  float mass = 1300.0f;                               // kg
  float wheelRadius = 0.35f, wheelWidth = 0.25f;
  float maxSteerDeg = 35.0f;
  float engineTorque = 500.0f, maxRpm = 6500.0f;      // Nm, rpm
  bool rearDrive = false;                             // tração traseira (falso = dianteira)
  float suspensionMin = 0.25f, suspensionMax = 0.45f; // m
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
  // Malha de triângulos parada (prédios, terreno, o cenário de um glTF), em coordenadas do mundo.
  uint32_t addStaticMesh(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices, float friction);

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
  // Editor: põe o corpo numa pose (teletransporte) e zera a velocidade.
  void setPose(uint32_t body, glm::vec3 position, glm::quat rotation);
  // Primeiro corpo atingido pelo raio (origem + direção normalizada × até maxDistance); ~0u = nenhum.
  uint32_t castRay(glm::vec3 origin, glm::vec3 direction, float maxDistance, float* hitDistance = nullptr) const;
  // Raio que ignora o corpo interno de um personagem (pés de quem anda: o raio sai de dentro da própria cápsula).
  // Devolve se acertou; hitDistance e hitNormal opcionais.
  bool castRayIgnoringCharacter(glm::vec3 origin, glm::vec3 direction, float maxDistance, uint32_t character,
                                float* hitDistance = nullptr, glm::vec3* hitNormal = nullptr) const;

  // Personagens (Jolt CharacterVirtual). moveCharacter vale para os passos seguintes: velocidade horizontal desejada
  // (m/s) e pedido de pulo (atendido no próximo passo com os pés no chão).
  uint32_t addCharacter(const CharacterDesc& desc, glm::vec3 feet);
  void moveCharacter(uint32_t character, glm::vec3 horizontalVelocity, bool jump, float jumpSpeed);
  glm::vec3 characterFeet(uint32_t character) const;
  glm::vec3 characterVelocity(uint32_t character) const;
  bool characterOnGround(uint32_t character) const;
  void setCharacterFeet(uint32_t character, glm::vec3 feet);  // editor: teletransporta e zera a velocidade

  // Veículos. drive: acelerador -1..1 (negativo = ré, com câmbio automático), direção -1..1 (+ = direita), freio e
  // freio de mão 0..1.
  uint32_t addVehicle(const VehicleDesc& desc, glm::vec3 position, glm::quat rotation);
  void driveVehicle(uint32_t vehicle, float forward, float right, float brake, float handBrake);
  uint32_t vehicleBody(uint32_t vehicle) const;  // corpo do chassi (pose(), linearVelocity())
  glm::mat4 wheelMatrix(uint32_t vehicle, int wheel) const;  // mundo; eixo da roda = +Y da malha
  float vehicleForwardSpeed(uint32_t vehicle) const;  // m/s na direção da frente
  float vehicleRpm(uint32_t vehicle) const;
  int vehicleGear(uint32_t vehicle) const;

  uint32_t bodyCount() const;
  uint32_t activeBodyCount() const;
  uint64_t steps() const;  // passos fixos já simulados
  static constexpr float kStep = 1.0f / 60.0f;

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
