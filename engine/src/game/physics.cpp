#include "game/physics.h"

// O Jolt.h vem antes de qualquer outro cabeçalho do Jolt.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

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

void Physics::optimizeBroadphase() { p_->system->OptimizeBroadPhase(); }

void Physics::update(float dt) {
  // No máximo 0,25 s por quadro: depois de uma travada longa a simulação não tenta recuperar tudo de uma vez.
  p_->accumulator += std::clamp(dt, 0.0f, 0.25f);
  while (p_->accumulator >= kStep) {
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
void Physics::addImpulse(uint32_t body, glm::vec3 impulse) { p_->bodies().AddImpulse(bodyId(body), toJolt(impulse)); }

void Physics::moveKinematic(uint32_t body, glm::vec3 position, glm::quat rotation) {
  p_->bodies().MoveKinematic(bodyId(body), JPH::RVec3(position.x, position.y, position.z), toJolt(rotation), kStep);
}

bool Physics::isActive(uint32_t body) const { return p_->bodies().IsActive(bodyId(body)); }
uint32_t Physics::bodyCount() const { return p_->system->GetNumBodies(); }
uint32_t Physics::activeBodyCount() const { return p_->system->GetNumActiveBodies(JPH::EBodyType::RigidBody); }
uint64_t Physics::steps() const { return p_->steps; }

}  // namespace game
