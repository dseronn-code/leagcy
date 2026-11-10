#include "game/physics.h"

// O Jolt.h vem antes de qualquer outro cabeçalho do Jolt.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockMulti.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/SwingTwistConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"

namespace game {

namespace {

// Duas camadas: o que não se move só testa contra o que se move (chão x chão nunca é testado).
constexpr JPH::ObjectLayer kLayerStatic = 0, kLayerMoving = 1;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
    return JPH::BroadPhaseLayer(JPH::BroadPhaseLayer::Type(layer == kLayerStatic ? 0 : 1));
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
    return layer.GetValue() == 0 ? "parado" : "movel";
  }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
    return layer != kLayerStatic || bp.GetValue() == 1;
  }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
    return a == kLayerMoving || b == kLayerMoving;
  }
};

void joltTrace(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  LOG_INFO("Jolt: %s", buf);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expr, const char* msg, const char* file, JPH::uint line) {
  LOG_ERROR("Jolt: falha em %s (%s) %s:%u", expr, msg ? msg : "", file, line);
  return true;  // para no depurador
}
#endif

// O alocador, a fábrica de tipos e o registro são globais no Jolt: uma vez por processo, com contagem de mundos.
std::mutex gInitMutex;
int gInitCount = 0;

void joltAcquire() {
  std::lock_guard<std::mutex> lock(gInitMutex);
  if (gInitCount++ > 0) return;
  JPH::RegisterDefaultAllocator();
  JPH::Trace = joltTrace;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed = joltAssert;
#endif
  JPH::Factory::sInstance = new JPH::Factory();
  JPH::RegisterTypes();
}

void joltRelease() {
  std::lock_guard<std::mutex> lock(gInitMutex);
  if (--gInitCount > 0) return;
  JPH::UnregisterTypes();
  delete JPH::Factory::sInstance;
  JPH::Factory::sInstance = nullptr;
}

JPH::Vec3 toJolt(glm::vec3 v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat toJolt(glm::quat q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::vec3 fromJolt(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat fromJolt(JPH::Quat q) { return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ()); }
JPH::BodyID bodyId(uint32_t id) { return JPH::BodyID(id); }

}  // namespace

struct Physics::Impl {
  BroadPhaseLayers broadPhaseLayers;
  ObjectVsBroadPhase objectVsBroadPhase;
  ObjectPairs objectPairs;
  std::unique_ptr<JPH::TempAllocator> temp;
  std::unique_ptr<JPH::JobSystemThreadPool> jobs;
  std::unique_ptr<JPH::PhysicsSystem> system;
  float accumulator = 0.0f;
  uint64_t steps = 0;
  int subSteps = 2;  // subpassos de colisão por passo fixo

  struct Character {
    JPH::Ref<JPH::CharacterVirtual> body;
    glm::vec3 desired{0.0f};
    float jumpBuffer = 0.0f;  // s em que um pulo pedido ainda vale (pedido um pouco antes de tocar o chão)
    float jumpSpeed = 5.0f;
    float stepHeight = 0.4f;
    float soloAccumulator = 0.0f;  // passos só deste personagem (previsão no cliente de rede)
  };
  std::vector<Character> characters;

  struct Vehicle {
    JPH::BodyID body;
    JPH::Ref<JPH::VehicleConstraint> constraint;
  };
  std::vector<Vehicle> vehicles;

  struct Ragdoll {
    std::vector<uint32_t> bodies;
    std::vector<JPH::Ref<JPH::Constraint>> joints;
  };
  std::vector<Ragdoll> ragdolls;
  JPH::CollisionGroup::GroupID nextGroup = 1;

  // Um passo de cada personagem (antes do passo dos corpos rígidos, como no exemplo do Jolt).
  void stepCharacters(float dt) {
    for (Character& c : characters) stepCharacter(c, dt);
  }
  void stepCharacter(Character& c, float dt) {
    const JPH::Vec3 up = JPH::Vec3::sAxisY(), gravity = system->GetGravity();
    {
      JPH::CharacterVirtual& ch = *c.body;
      ch.UpdateGroundVelocity();
      const JPH::Vec3 current = ch.GetLinearVelocity();
      const JPH::Vec3 ground = ch.GetGroundVelocity();
      const JPH::Vec3 vertical = up * current.Dot(up);
      // No chão (e não subindo por um pulo): acompanha o chão (plataformas) e pode pular.
      const bool onGround = ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround &&
                            (current - ground).Dot(up) < 0.1f;
      JPH::Vec3 v = onGround ? ground : vertical;
      if (onGround && c.jumpBuffer > 0.0f) {
        v += up * c.jumpSpeed;
        c.jumpBuffer = 0.0f;
      }
      c.jumpBuffer = std::max(c.jumpBuffer - dt, 0.0f);
      v += toJolt(c.desired) + gravity * dt;
      ch.SetLinearVelocity(v);
      JPH::CharacterVirtual::ExtendedUpdateSettings settings;
      settings.mWalkStairsStepUp = JPH::Vec3(0.0f, c.stepHeight, 0.0f);
      ch.ExtendedUpdate(dt, gravity, settings, JPH::DefaultBroadPhaseLayerFilter(objectVsBroadPhase, kLayerMoving),
                        JPH::DefaultObjectLayerFilter(objectPairs, kLayerMoving), JPH::BodyFilter(), JPH::ShapeFilter(),
                        *temp);
    }
  }

  JPH::BodyInterface& bodies() { return system->GetBodyInterface(); }
  const JPH::BodyInterface& bodies() const { return system->GetBodyInterface(); }

  uint32_t add(const JPH::Shape* shape, glm::vec3 position, glm::quat rotation, const BodyDesc& d) {
    const JPH::EMotionType type = d.motion == Motion::Static    ? JPH::EMotionType::Static
                                  : d.motion == Motion::Dynamic ? JPH::EMotionType::Dynamic
                                                                : JPH::EMotionType::Kinematic;
    JPH::BodyCreationSettings s(shape, JPH::RVec3(position.x, position.y, position.z), toJolt(rotation), type,
                                d.motion == Motion::Static ? kLayerStatic : kLayerMoving);
    s.mFriction = d.friction;
    s.mRestitution = d.restitution;
    s.mLinearDamping = d.linearDamping;
    s.mAngularDamping = d.angularDamping;
    if (d.motion == Motion::Dynamic && d.mass > 0.0f) {
      s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
      s.mMassPropertiesOverride.mMass = d.mass;
    }
    const JPH::BodyID id = bodies().CreateAndAddBody(
        s, d.motion == Motion::Static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
    if (id.IsInvalid()) {
      LOG_ERROR("Física: limite de corpos atingido");
      return ~0u;
    }
    return id.GetIndexAndSequenceNumber();
  }
};

Physics::Physics(uint32_t maxBodies) : p_(std::make_unique<Impl>()) {
  joltAcquire();
  // Memória de rascunho de cada passo: 32 MB fixos e, em cenas com contatos demais, o malloc em vez de abortar.
  p_->temp = std::make_unique<JPH::TempAllocatorImplWithMallocFallback>(32u * 1024u * 1024u);
  const int threads = std::max(1, int(std::thread::hardware_concurrency()) - 1);
  p_->jobs = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, threads);
  p_->system = std::make_unique<JPH::PhysicsSystem>();
  // Pares no broadphase e contatos simultâneos: os limites crescem com os corpos, até 64 mil / 32 mil.
  const JPH::uint pairs = std::min<JPH::uint>(maxBodies * 2u, 65536u), contacts = std::min<JPH::uint>(maxBodies, 32768u);
  p_->system->Init(maxBodies, 0, pairs, contacts, p_->broadPhaseLayers, p_->objectVsBroadPhase, p_->objectPairs);
  // Estabilidade de pilhas (tools/fisica.py, torre de 12 caixas parada 10 s): o padrão do Jolt (10 iterações de
  // velocidade, 2 de posição, 1 subpasso de 1/60 s) deixava o topo inclinar 46 cm; 20/4 com 2 subpassos (1/120 s)
  // deixa 0,2 cm. LEGACY_FIS_PASSOS="v,p" e LEGACY_FIS_SUBPASSOS=n mudam isso (medição).
  JPH::PhysicsSettings ps = p_->system->GetPhysicsSettings();
  ps.mNumVelocitySteps = 20;
  ps.mNumPositionSteps = 4;
  if (const char* e = std::getenv("LEGACY_FIS_PASSOS")) {
    int v = 0, q = 0;
    if (std::sscanf(e, "%d,%d", &v, &q) == 2 && v > 0 && q > 0) {
      ps.mNumVelocitySteps = JPH::uint(v);
      ps.mNumPositionSteps = JPH::uint(q);
    }
  }
  p_->system->SetPhysicsSettings(ps);
  p_->subSteps = 2;
  if (const char* e = std::getenv("LEGACY_FIS_SUBPASSOS")) p_->subSteps = std::max(1, std::atoi(e));
}

Physics::~Physics() {
  for (auto& v : p_->vehicles) {  // restrições antes dos corpos
    p_->system->RemoveStepListener(v.constraint);
    p_->system->RemoveConstraint(v.constraint);
  }
  p_->vehicles.clear();
  for (auto& r : p_->ragdolls)
    for (auto& j : r.joints) p_->system->RemoveConstraint(j);
  p_->ragdolls.clear();
  p_->characters.clear();  // antes dos corpos: o personagem tem um corpo interno no sistema
  if (p_->system) {
    JPH::BodyIDVector ids;
    p_->system->GetBodies(ids);
    if (!ids.empty()) {
      p_->bodies().RemoveBodies(ids.data(), int(ids.size()));
      p_->bodies().DestroyBodies(ids.data(), int(ids.size()));
    }
  }
  p_->system.reset();
  p_->jobs.reset();
  p_->temp.reset();
  joltRelease();
}

uint32_t Physics::addBox(glm::vec3 halfExtents, glm::vec3 position, glm::quat rotation, const BodyDesc& desc) {
  halfExtents = glm::max(halfExtents, glm::vec3(1e-3f));
  // Raio de arredondamento das quinas: o padrão do Jolt (5 cm), menor em caixas finas.
  const float convex = std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({halfExtents.x, halfExtents.y, halfExtents.z}));
  JPH::BoxShapeSettings settings(toJolt(halfExtents), convex);
  JPH::ShapeSettings::ShapeResult r = settings.Create();
  if (r.HasError()) {
    LOG_ERROR("Física: caixa inválida (%s)", r.GetError().c_str());
    return ~0u;
  }
  return p_->add(r.Get(), position, rotation, desc);
}

uint32_t Physics::addSphere(float radius, glm::vec3 position, glm::quat rotation, const BodyDesc& desc) {
  return p_->add(new JPH::SphereShape(std::max(radius, 1e-3f)), position, rotation, desc);
}

uint32_t Physics::addCapsule(float halfHeight, float radius, glm::vec3 position, glm::quat rotation,
                             const BodyDesc& desc) {
  return p_->add(new JPH::CapsuleShape(std::max(halfHeight, 1e-3f), std::max(radius, 1e-3f)), position, rotation, desc);
}

uint32_t Physics::addStaticMesh(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices,
                                float friction) {
  JPH::VertexList verts;
  verts.reserve(vertices.size());
  for (const glm::vec3& v : vertices) verts.push_back(JPH::Float3(v.x, v.y, v.z));
  JPH::IndexedTriangleList tris;
  tris.reserve(indices.size() / 3);
  for (size_t i = 0; i + 2 < indices.size(); i += 3) {
    const glm::vec3 a = vertices[indices[i]], b = vertices[indices[i + 1]], c = vertices[indices[i + 2]];
    if (glm::dot(glm::cross(b - a, c - a), glm::cross(b - a, c - a)) < 1e-14f) continue;  // degenerado
    tris.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2], 0));
  }
  JPH::MeshShapeSettings settings(std::move(verts), std::move(tris));
  JPH::ShapeSettings::ShapeResult r = settings.Create();
  if (r.HasError()) {
    LOG_ERROR("Física: malha de colisão inválida (%s)", r.GetError().c_str());
    return ~0u;
  }
  BodyDesc d;
  d.friction = friction;
  return p_->add(r.Get(), glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), d);
}

namespace {
// Menor distância entre os segmentos pq e rs (amostrado: as partes do ragdoll são poucas e isso roda uma vez).
float segmentDistance(glm::vec3 p, glm::vec3 q, glm::vec3 r, glm::vec3 s) {
  float best = 1e30f;
  for (int i = 0; i <= 16; ++i)
    for (int j = 0; j <= 16; ++j)
      best = std::min(best, glm::length(glm::mix(p, q, float(i) / 16.0f) - glm::mix(r, s, float(j) / 16.0f)));
  return best;
}
// Rotação que leva +Y para `dir` (normalizado).
glm::quat yTo(glm::vec3 dir) {
  const glm::vec3 y(0.0f, 1.0f, 0.0f);
  const float d = glm::dot(y, dir);
  if (d > 0.9999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
  if (d < -0.9999f) return glm::angleAxis(glm::pi<float>(), glm::vec3(1.0f, 0.0f, 0.0f));
  return glm::normalize(glm::quat(1.0f + d, glm::cross(y, dir)));
}
}  // namespace

std::vector<uint32_t> Physics::addRagdoll(const std::vector<RagdollPart>& parts, glm::vec3 velocity) {
  std::vector<uint32_t> out;
  if (parts.empty()) return out;
  const JPH::uint n = JPH::uint(parts.size());
  JPH::Ref<JPH::GroupFilterTable> filter = new JPH::GroupFilterTable(n);
  for (JPH::uint i = 0; i < n; ++i)
    for (JPH::uint j = i + 1; j < n; ++j) {
      const RagdollPart &a = parts[i], &b = parts[j];
      const bool family = a.parent == int(j) || b.parent == int(i) || (a.parent >= 0 && a.parent == b.parent);
      if (family || segmentDistance(a.a, a.b, b.a, b.b) < 1.1f * (a.radius + b.radius)) filter->DisableCollision(i, j);
    }
  const JPH::CollisionGroup::GroupID group = p_->nextGroup++;
  Impl::Ragdoll rd;
  for (JPH::uint i = 0; i < n; ++i) {
    const RagdollPart& part = parts[i];
    const glm::vec3 axis = part.b - part.a;
    const float len = glm::length(axis);
    const float r = std::max(part.radius, 1e-3f);
    const float half = 0.5f * len - r;
    JPH::Ref<JPH::Shape> shape;
    if (half > 1e-3f) shape = new JPH::CapsuleShape(half, r);
    else shape = new JPH::SphereShape(std::max(r, 0.5f * len));
    const glm::vec3 center = 0.5f * (part.a + part.b);
    const glm::quat rot = len > 1e-5f ? yTo(axis / len) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    JPH::BodyCreationSettings s(shape, JPH::RVec3(center.x, center.y, center.z), toJolt(rot), JPH::EMotionType::Dynamic,
                                kLayerMoving);
    s.mFriction = 0.8f;
    s.mRestitution = 0.0f;
    s.mLinearDamping = 0.05f;
    s.mAngularDamping = 0.6f;
    s.mMotionQuality = JPH::EMotionQuality::LinearCast;  // partes finas caindo rápido não atravessam o chão
    s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    s.mMassPropertiesOverride.mMass = std::max(part.mass, 0.01f);
    s.mCollisionGroup = JPH::CollisionGroup(filter, group, i);
    s.mLinearVelocity = toJolt(velocity);
    const JPH::BodyID id = p_->bodies().CreateAndAddBody(s, JPH::EActivation::Activate);
    if (id.IsInvalid()) {
      LOG_ERROR("Física: limite de corpos atingido (ragdoll)");
      removeRagdoll(out);
      return {};
    }
    out.push_back(id.GetIndexAndSequenceNumber());
  }
  for (JPH::uint i = 0; i < n; ++i) {
    const RagdollPart& part = parts[i];
    if (part.parent < 0 || part.parent >= int(n)) continue;
    glm::vec3 twist = part.b - part.a;
    twist = glm::length(twist) > 1e-6f ? glm::normalize(twist) : glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 plane = glm::cross(twist, std::abs(twist.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f));
    plane = glm::normalize(plane);
    JPH::SwingTwistConstraintSettings cs;
    cs.mSpace = JPH::EConstraintSpace::WorldSpace;
    cs.mPosition1 = cs.mPosition2 = JPH::RVec3(part.a.x, part.a.y, part.a.z);
    cs.mTwistAxis1 = cs.mTwistAxis2 = toJolt(twist);
    cs.mPlaneAxis1 = cs.mPlaneAxis2 = toJolt(plane);
    cs.mNormalHalfConeAngle = cs.mPlaneHalfConeAngle = glm::radians(part.coneDeg);
    cs.mTwistMinAngle = -glm::radians(part.twistDeg);
    cs.mTwistMaxAngle = glm::radians(part.twistDeg);
    // Atrito na junta: um pouco de resistência (músculo relaxado), sem deixar o corpo duro.
    cs.mMaxFrictionTorque = 0.05f * part.mass * 9.81f * glm::length(part.b - part.a);
    const std::array<JPH::BodyID, 2> pair{bodyId(out[size_t(part.parent)]), bodyId(out[i])};
    JPH::BodyLockMultiWrite lock(p_->system->GetBodyLockInterface(), pair.data(), 2);
    JPH::Body* b1 = lock.GetBody(0);
    JPH::Body* b2 = lock.GetBody(1);
    if (!b1 || !b2) continue;
    JPH::Ref<JPH::Constraint> c = cs.Create(*b1, *b2);
    rd.joints.push_back(c);
  }
  for (auto& c : rd.joints) p_->system->AddConstraint(c);
  rd.bodies = out;
  p_->ragdolls.push_back(std::move(rd));
  return out;
}

void Physics::removeRagdoll(const std::vector<uint32_t>& bodies) {
  if (bodies.empty()) return;
  for (size_t k = 0; k < p_->ragdolls.size(); ++k) {
    if (p_->ragdolls[k].bodies.empty() || p_->ragdolls[k].bodies[0] != bodies[0]) continue;
    for (auto& j : p_->ragdolls[k].joints) p_->system->RemoveConstraint(j);
    p_->ragdolls.erase(p_->ragdolls.begin() + std::ptrdiff_t(k));
    break;
  }
  std::vector<JPH::BodyID> ids;
  for (uint32_t b : bodies) ids.push_back(bodyId(b));
  p_->bodies().RemoveBodies(ids.data(), int(ids.size()));
  p_->bodies().DestroyBodies(ids.data(), int(ids.size()));
}

void Physics::optimizeBroadphase() { p_->system->OptimizeBroadPhase(); }

void Physics::update(float dt) {
  // No máximo 0,25 s por quadro: depois de uma travada longa a simulação não tenta recuperar tudo de uma vez.
  p_->accumulator += std::clamp(dt, 0.0f, 0.25f);
  while (p_->accumulator >= kStep) {
    p_->stepCharacters(kStep);
    p_->system->Update(kStep, p_->subSteps, p_->temp.get(), p_->jobs.get());
    p_->accumulator -= kStep;
    ++p_->steps;
  }
}

float Physics::interpolation() const { return p_->accumulator / kStep; }

void Physics::pose(uint32_t body, glm::vec3& position, glm::quat& rotation) const {
  JPH::RVec3 p;
  JPH::Quat q;
  p_->bodies().GetPositionAndRotation(bodyId(body), p, q);
  position = glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
  rotation = fromJolt(q);
}

glm::vec3 Physics::linearVelocity(uint32_t body) const { return fromJolt(p_->bodies().GetLinearVelocity(bodyId(body))); }
void Physics::setLinearVelocity(uint32_t body, glm::vec3 v) { p_->bodies().SetLinearVelocity(bodyId(body), toJolt(v)); }
glm::vec3 Physics::angularVelocity(uint32_t body) const { return fromJolt(p_->bodies().GetAngularVelocity(bodyId(body))); }
void Physics::setAngularVelocity(uint32_t body, glm::vec3 v) { p_->bodies().SetAngularVelocity(bodyId(body), toJolt(v)); }
void Physics::setCharacterVelocity(uint32_t i, glm::vec3 v) {
  if (i < p_->characters.size()) p_->characters[i].body->SetLinearVelocity(toJolt(v));
}
void Physics::addImpulse(uint32_t body, glm::vec3 impulse) { p_->bodies().AddImpulse(bodyId(body), toJolt(impulse)); }

void Physics::moveKinematic(uint32_t body, glm::vec3 position, glm::quat rotation) {
  p_->bodies().MoveKinematic(bodyId(body), JPH::RVec3(position.x, position.y, position.z), toJolt(rotation), kStep);
}

uint32_t Physics::addCharacter(const CharacterDesc& d, glm::vec3 feet) {
  const float radius = std::max(d.radius, 0.05f);
  const float halfHeight = std::max(0.5f * d.height - radius, 0.05f);
  // Cápsula com os pés na origem do personagem.
  JPH::RotatedTranslatedShapeSettings shapeSettings(JPH::Vec3(0.0f, halfHeight + radius, 0.0f), JPH::Quat::sIdentity(),
                                                    new JPH::CapsuleShape(halfHeight, radius));
  JPH::ShapeSettings::ShapeResult r = shapeSettings.Create();
  if (r.HasError()) {
    LOG_ERROR("Física: personagem inválido (%s)", r.GetError().c_str());
    return ~0u;
  }
  JPH::Ref<JPH::CharacterVirtualSettings> s = new JPH::CharacterVirtualSettings();
  s->mShape = r.Get();
  s->mMaxSlopeAngle = glm::radians(d.maxSlopeDeg);
  s->mMass = d.mass;
  s->mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -radius);  // só contatos perto dos pés sustentam
  s->mInnerBodyShape = r.Get();  // corpo interno: caixas e bolas batem no personagem
  s->mInnerBodyLayer = kLayerMoving;
  Impl::Character c;
  c.body = new JPH::CharacterVirtual(s, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(), 0, p_->system.get());
  c.stepHeight = d.stepHeight;
  p_->characters.push_back(c);
  return uint32_t(p_->characters.size() - 1);
}

void Physics::stepCharacterOnly(uint32_t i, float dt) {
  if (i >= p_->characters.size()) return;
  Impl::Character& c = p_->characters[i];
  c.soloAccumulator += std::clamp(dt, 0.0f, 0.25f);
  while (c.soloAccumulator >= kStep) {  // o mesmo passo fixo do anfitrião: a previsão anda igual
    p_->stepCharacter(c, kStep);
    c.soloAccumulator -= kStep;
  }
}

void Physics::nudgeCharacter(uint32_t i, glm::vec3 delta) {
  if (i >= p_->characters.size()) return;
  JPH::CharacterVirtual& c = *p_->characters[i].body;
  c.SetPosition(c.GetPosition() + JPH::RVec3(delta.x, delta.y, delta.z));  // a velocidade fica
}

void Physics::moveCharacter(uint32_t i, glm::vec3 horizontalVelocity, bool jump, float jumpSpeed) {
  if (i >= p_->characters.size()) return;
  Impl::Character& c = p_->characters[i];
  c.desired = glm::vec3(horizontalVelocity.x, 0.0f, horizontalVelocity.z);
  if (jump) c.jumpBuffer = 0.15f;
  c.jumpSpeed = jumpSpeed;
}

glm::vec3 Physics::characterFeet(uint32_t i) const {
  const JPH::RVec3 p = p_->characters[i].body->GetPosition();
  return glm::vec3(float(p.GetX()), float(p.GetY()), float(p.GetZ()));
}
glm::vec3 Physics::characterVelocity(uint32_t i) const { return fromJolt(p_->characters[i].body->GetLinearVelocity()); }
void Physics::setCharacterFeet(uint32_t i, glm::vec3 feet) {
  if (i >= p_->characters.size()) return;
  JPH::CharacterVirtual& c = *p_->characters[i].body;
  c.SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
  c.SetLinearVelocity(JPH::Vec3::sZero());
}

uint32_t Physics::addVehicle(const VehicleDesc& d, glm::vec3 position, glm::quat rotation) {
  const float hw = 0.5f * d.width, hh = 0.5f * d.height, hl = 0.5f * d.length;
  // Chassi com o centro de massa baixo (estável nas curvas, como um carro de verdade).
  JPH::OffsetCenterOfMassShapeSettings shapeSettings(JPH::Vec3(0.0f, -hh, 0.0f), new JPH::BoxShape(JPH::Vec3(hw, hh, hl)));
  JPH::ShapeSettings::ShapeResult r = shapeSettings.Create();
  if (r.HasError()) {
    LOG_ERROR("Física: chassi inválido (%s)", r.GetError().c_str());
    return ~0u;
  }
  JPH::BodyCreationSettings bs(r.Get(), JPH::RVec3(position.x, position.y, position.z), toJolt(rotation),
                               JPH::EMotionType::Dynamic, kLayerMoving);
  bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
  bs.mMassPropertiesOverride.mMass = d.mass;
  const JPH::BodyID body = p_->bodies().CreateAndAddBody(bs, JPH::EActivation::Activate);
  if (body.IsInvalid()) return ~0u;

  JPH::VehicleConstraintSettings vs;
  vs.mMaxPitchRollAngle = JPH::DegreesToRadians(60.0f);  // não capota com facilidade
  const float wheelY = -0.9f * hh, frontZ = hl - 1.8f * d.wheelRadius, rearZ = -hl + 1.8f * d.wheelRadius;
  const JPH::Vec3 positions[4] = {{hw, wheelY, frontZ}, {-hw, wheelY, frontZ}, {hw, wheelY, rearZ}, {-hw, wheelY, rearZ}};
  for (int i = 0; i < 4; ++i) {
    JPH::WheelSettingsWV* w = new JPH::WheelSettingsWV;
    w->mPosition = positions[i];
    w->mRadius = d.wheelRadius;
    w->mWidth = d.wheelWidth;
    w->mSuspensionMinLength = d.suspensionMin;
    w->mSuspensionMaxLength = d.suspensionMax;
    w->mMaxSteerAngle = i < 2 ? JPH::DegreesToRadians(d.maxSteerDeg) : 0.0f;  // direção só nas dianteiras
    w->mMaxHandBrakeTorque = i < 2 ? 0.0f : 4000.0f;                          // freio de mão só nas traseiras
    vs.mWheels.push_back(w);
  }
  JPH::WheeledVehicleControllerSettings* controller = new JPH::WheeledVehicleControllerSettings;
  controller->mEngine.mMaxTorque = d.engineTorque;
  controller->mEngine.mMaxRPM = d.maxRpm;
  controller->mDifferentials.resize(1);
  controller->mDifferentials[0].mLeftWheel = d.rearDrive ? 2 : 0;
  controller->mDifferentials[0].mRightWheel = d.rearDrive ? 3 : 1;
  vs.mController = controller;
  // Barras estabilizadoras nos dois eixos (menos rolagem da carroceria).
  vs.mAntiRollBars.resize(2);
  vs.mAntiRollBars[0].mLeftWheel = 0;
  vs.mAntiRollBars[0].mRightWheel = 1;
  vs.mAntiRollBars[1].mLeftWheel = 2;
  vs.mAntiRollBars[1].mRightWheel = 3;

  JPH::BodyLockWrite lock(p_->system->GetBodyLockInterface(), body);
  if (!lock.Succeeded()) return ~0u;
  Impl::Vehicle v;
  v.body = body;
  v.constraint = new JPH::VehicleConstraint(lock.GetBody(), vs);
  v.constraint->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterCastCylinder(kLayerMoving));
  p_->system->AddConstraint(v.constraint);
  p_->system->AddStepListener(v.constraint);
  p_->vehicles.push_back(v);
  return uint32_t(p_->vehicles.size() - 1);
}

void Physics::driveVehicle(uint32_t i, float forward, float right, float brake, float handBrake) {
  if (i >= p_->vehicles.size()) return;
  auto* c = static_cast<JPH::WheeledVehicleController*>(p_->vehicles[i].constraint->GetController());
  c->SetDriverInput(std::clamp(forward, -1.0f, 1.0f), std::clamp(right, -1.0f, 1.0f), std::clamp(brake, 0.0f, 1.0f),
                    std::clamp(handBrake, 0.0f, 1.0f));
  if (forward != 0.0f || right != 0.0f || brake != 0.0f || handBrake != 0.0f) p_->bodies().ActivateBody(p_->vehicles[i].body);
}

uint32_t Physics::vehicleBody(uint32_t i) const {
  return i < p_->vehicles.size() ? p_->vehicles[i].body.GetIndexAndSequenceNumber() : ~0u;
}

glm::mat4 Physics::wheelMatrix(uint32_t i, int wheel) const {
  const JPH::RMat44 m = p_->vehicles[i].constraint->GetWheelWorldTransform(JPH::uint(wheel), JPH::Vec3::sAxisY(), JPH::Vec3::sAxisX());
  glm::mat4 out(1.0f);
  for (int c = 0; c < 4; ++c) {
    const JPH::Vec4 col = c < 3 ? JPH::Vec4(m.GetColumn3(c), 0.0f) : JPH::Vec4(JPH::Vec3(m.GetTranslation()), 1.0f);
    out[c] = glm::vec4(col.GetX(), col.GetY(), col.GetZ(), col.GetW());
  }
  return out;
}

float Physics::vehicleForwardSpeed(uint32_t i) const {
  const JPH::BodyID b = p_->vehicles[i].body;
  const JPH::Vec3 fwd = p_->bodies().GetRotation(b) * JPH::Vec3::sAxisZ();
  return p_->bodies().GetLinearVelocity(b).Dot(fwd);
}

float Physics::vehicleRpm(uint32_t i) const {
  return static_cast<const JPH::WheeledVehicleController*>(p_->vehicles[i].constraint->GetController())->GetEngine().GetCurrentRPM();
}

int Physics::vehicleGear(uint32_t i) const {
  return static_cast<const JPH::WheeledVehicleController*>(p_->vehicles[i].constraint->GetController())->GetTransmission().GetCurrentGear();
}

bool Physics::characterOnGround(uint32_t i) const {
  return p_->characters[i].body->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
}

void Physics::setPose(uint32_t body, glm::vec3 position, glm::quat rotation) {
  JPH::BodyInterface& bi = p_->bodies();
  const JPH::BodyID id = bodyId(body);
  const bool moving = bi.GetMotionType(id) != JPH::EMotionType::Static;
  bi.SetPositionAndRotation(id, JPH::RVec3(position.x, position.y, position.z), toJolt(rotation),
                            moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
  if (moving) {
    bi.SetLinearVelocity(id, JPH::Vec3::sZero());
    bi.SetAngularVelocity(id, JPH::Vec3::sZero());
  }
}

uint32_t Physics::castRay(glm::vec3 origin, glm::vec3 direction, float maxDistance, float* hitDistance) const {
  const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(direction * maxDistance));
  JPH::RayCastResult hit;
  if (!p_->system->GetNarrowPhaseQuery().CastRay(ray, hit)) return ~0u;
  if (hitDistance) *hitDistance = hit.mFraction * maxDistance;
  return hit.mBodyID.GetIndexAndSequenceNumber();
}

bool Physics::castRayIgnoringCharacter(glm::vec3 origin, glm::vec3 direction, float maxDistance, uint32_t character,
                                       float* hitDistance, glm::vec3* hitNormal) const {
  const JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(direction * maxDistance));
  JPH::RayCastResult hit;
  JPH::BodyID ignore;
  if (character < p_->characters.size()) ignore = p_->characters[character].body->GetInnerBodyID();
  const JPH::IgnoreSingleBodyFilter filter(ignore);
  if (!p_->system->GetNarrowPhaseQuery().CastRay(ray, hit, {}, {}, filter)) return false;
  if (hitDistance) *hitDistance = hit.mFraction * maxDistance;
  if (hitNormal) {
    JPH::BodyLockRead lock(p_->system->GetBodyLockInterface(), hit.mBodyID);
    *hitNormal = glm::vec3(0.0f, 1.0f, 0.0f);
    if (lock.Succeeded())
      *hitNormal = fromJolt(lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, ray.GetPointOnRay(hit.mFraction)));
  }
  return true;
}

bool Physics::isActive(uint32_t body) const { return p_->bodies().IsActive(bodyId(body)); }
uint32_t Physics::bodyCount() const { return p_->system->GetNumBodies(); }
uint32_t Physics::activeBodyCount() const { return p_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody); }
uint64_t Physics::steps() const { return p_->steps; }

}  // namespace game
