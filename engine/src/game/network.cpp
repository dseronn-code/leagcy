#include "game/network.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include <enet/enet.h>
#include <glm/gtc/quaternion.hpp>

#include "core/log.h"
#include "game/world.h"
#include "scene/scene.h"

namespace game {

namespace {

enum MsgType : uint8_t { kInput = 1, kWelcome = 2, kState = 3 };
constexpr float kSendInterval = 1.0f / 30.0f;  // estado a 30 Hz
constexpr float kInterpDelay = 0.1f;           // o cliente mostra o mundo 100 ms atrás (entre dois estados)

struct Writer {
  std::vector<uint8_t> b;
  template <class T>
  void put(const T& v) {
    const size_t n = b.size();
    b.resize(n + sizeof(T));
    std::memcpy(b.data() + n, &v, sizeof(T));
  }
  void str(const std::string& s) {
    put(uint16_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
  }
};
struct Reader {
  const uint8_t* p;
  size_t n, i = 0;
  bool ok = true;
  template <class T>
  T get() {
    T v{};
    if (i + sizeof(T) > n) { ok = false; return v; }
    std::memcpy(&v, p + i, sizeof(T));
    i += sizeof(T);
    return v;
  }
  std::string str() {
    const uint16_t len = get<uint16_t>();
    if (!ok || i + len > n) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p + i), len);
    i += len;
    return s;
  }
};

// Entidades replicadas: tudo com nome que se mexe (corpos não estáticos, personagens, carros), na ordem do registro.
std::vector<entt::entity> replicated(const World& w) {
  std::vector<entt::entity> out;
  for (auto e : w.registry.view<Name, Transform>()) {
    if (w.registry.all_of<SystemEntity>(e)) continue;
    const RigidBody* rb = w.registry.try_get<RigidBody>(e);
    const bool moves = w.registry.all_of<Character>(e) || w.registry.all_of<Vehicle>(e) ||
                       (rb && rb->desc.motion != Motion::Static);
    if (moves) out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](entt::entity a, entt::entity b) { return entt::to_integral(a) < entt::to_integral(b); });
  return out;
}

struct Pose {
  glm::vec3 p;
  glm::quat r;
};
struct Snapshot {
  float hostTime;
  std::vector<Pose> poses;
};

}  // namespace

struct Network::Impl {
  ENetHost* host = nullptr;
  ENetPeer* server = nullptr;  // cliente: conexão com o anfitrião
  bool isHost = false, isClient = false;
  // Anfitrião
  std::map<ENetPeer*, int> slotOf;
  std::vector<entt::entity> table;  // ordem da tabela de nomes
  float sendTimer = 0.0f;
  // Cliente
  std::vector<entt::entity> clientTable;  // índice do anfitrião -> entidade local (null se não existe aqui)
  std::deque<Snapshot> snaps;
  float renderTime = -1.0f;
  bool welcomed = false;

  ~Impl() {
    if (server) enet_peer_disconnect_now(server, 0);
    if (host) {
      enet_host_flush(host);
      enet_host_destroy(host);
    }
  }

  static void send(ENetPeer* peer, const Writer& w, uint8_t channel, bool reliable) {
    ENetPacket* pk = enet_packet_create(w.b.data(), w.b.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
    enet_peer_send(peer, channel, pk);
  }

  void welcome(World& world, ENetPeer* peer, int slot) {
    Writer w;
    w.put(uint8_t(kWelcome));
    w.put(int32_t(slot));
    w.put(uint32_t(table.size()));
    for (entt::entity e : table) w.str(world.registry.get<Name>(e).value);
    send(peer, w, 0, true);
  }

  void hostUpdate(World& world, float dt) {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_CONNECT) {
        int slot = -1;
        for (size_t i = 0; i < world.slotActive.size(); ++i)
          if (!world.slotActive[i]) { slot = int(i); break; }
        if (slot < 0) {
          LOG_WARN("Multijogador: sem vaga livre; conexão recusada");
          enet_peer_disconnect(ev.peer, 0);
          continue;
        }
        slotOf[ev.peer] = slot;
        world.activateSlot(slot, true);
        welcome(world, ev.peer, slot);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        if (auto it = slotOf.find(ev.peer); it != slotOf.end()) {
          world.activateSlot(it->second, false);
          slotOf.erase(it);
        }
      } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        Reader r{ev.packet->data, ev.packet->dataLength};
        if (r.get<uint8_t>() == kInput) {
          auto it = slotOf.find(ev.peer);
          PlayerInput in;
          in.move.x = r.get<float>();
          in.move.y = r.get<float>();
          in.cameraYaw = r.get<float>();
          const uint8_t flags = r.get<uint8_t>();
          in.jump = flags & 1;
          in.run = flags & 2;
          if (r.ok && it != slotOf.end()) {
            if (glm::length(in.move) > 1.0f) in.move = glm::normalize(in.move);  // não confia no cliente
            world.remoteInputs[size_t(it->second)] = in;
          }
        }
        enet_packet_destroy(ev.packet);
      }
    }
    sendTimer += dt;
    if (sendTimer < kSendInterval || slotOf.empty()) return;
    sendTimer = 0.0f;
    Writer w;
    w.put(uint8_t(kState));
    w.put(world.time);
    w.put(uint32_t(table.size()));
    for (uint32_t i = 0; i < table.size(); ++i) {
      if (!world.registry.valid(table[i])) continue;
      const Transform& t = world.registry.get<Transform>(table[i]);
      w.put(uint16_t(i));
      w.put(t.position);
      w.put(t.rotation.w);
      w.put(t.rotation.x);
      w.put(t.rotation.y);
      w.put(t.rotation.z);
    }
    ENetPacket* pk = enet_packet_create(w.b.data(), w.b.size(), 0);  // não confiável: o próximo estado substitui
    enet_host_broadcast(host, 1, pk);
  }

  void applyPose(World& world, scene::Scene& scene, entt::entity e, const Pose& pose) {
    Transform& t = world.registry.get<Transform>(e);
    if (Character* c = world.registry.try_get<Character>(e)) {
      t.position = pose.p;
      if (c->id != ~0u) world.physics->setCharacterFeet(c->id, pose.p - glm::vec3(0.0f, 0.5f * c->desc.height, 0.0f));
      if (const Renderable* r = world.registry.try_get<Renderable>(e); r && world.registry.all_of<Shape>(e))
        scene.setTransform(r->sceneObject, world.entityMatrix(e));
      return;
    }
    if (world.registry.all_of<Vehicle>(e)) {
      t.position = pose.p;
      t.rotation = pose.r;
      if (const Renderable* r = world.registry.try_get<Renderable>(e)) scene.setTransform(r->sceneObject, world.entityMatrix(e));
      return;
    }
    world.setEntityPose(scene, e, pose.p, pose.r);
  }

  void clientUpdate(World& world, scene::Scene& scene, float dt, const PlayerInput& input) {
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        LOG_WARN("Multijogador: o anfitrião desconectou");
        server = nullptr;
      } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        Reader r{ev.packet->data, ev.packet->dataLength};
        const uint8_t type = r.get<uint8_t>();
        if (type == kWelcome) {
          world.localSlot = r.get<int32_t>();
          const uint32_t n = r.get<uint32_t>();
          std::map<std::string, entt::entity> byName;
          for (auto e : world.registry.view<Name>()) byName[world.registry.get<Name>(e).value] = e;
          clientTable.assign(n, entt::null);
          for (uint32_t i = 0; i < n && r.ok; ++i) {
            auto it = byName.find(r.str());
            if (it != byName.end()) clientTable[i] = it->second;
          }
          welcomed = r.ok;
          LOG_INFO("Multijogador: conectado na vaga %d (%u entidades replicadas)", world.localSlot, n);
        } else if (type == kState && welcomed) {
          Snapshot s;
          s.hostTime = r.get<float>();
          const uint32_t n = r.get<uint32_t>();
          s.poses.assign(clientTable.size(), Pose{glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)});
          for (uint32_t k = 0; k < n && r.ok; ++k) {
            const uint16_t i = r.get<uint16_t>();
            Pose p;
            p.p = r.get<glm::vec3>();
            p.r.w = r.get<float>();
            p.r.x = r.get<float>();
            p.r.y = r.get<float>();
            p.r.z = r.get<float>();
            if (r.ok && i < s.poses.size()) s.poses[i] = p;
          }
          if (r.ok && (snaps.empty() || s.hostTime > snaps.back().hostTime)) snaps.push_back(std::move(s));
          while (snaps.size() > 32) snaps.pop_front();
        }
        enet_packet_destroy(ev.packet);
      }
    }
    if (server && welcomed) {  // a entrada do jogador, todo quadro
      Writer w;
      w.put(uint8_t(kInput));
      w.put(input.move.x);
      w.put(input.move.y);
      w.put(input.cameraYaw);
      w.put(uint8_t((input.jump ? 1 : 0) | (input.run ? 2 : 0)));
      send(server, w, 1, false);
    }
    if (snaps.empty()) return;
    // Interpolação: o relógio de exibição anda com o quadro e fica ~100 ms atrás do último estado recebido.
    const float latest = snaps.back().hostTime;
    renderTime = renderTime < 0.0f ? latest - kInterpDelay : renderTime + dt;
    renderTime = std::clamp(renderTime, latest - 0.5f, latest);  // não fica muito atrás nem passa do último
    if (renderTime < latest - kInterpDelay - 0.05f) renderTime += 0.1f * dt;  // alcança devagar
    size_t b = 0;
    while (b < snaps.size() && snaps[b].hostTime < renderTime) ++b;
    const Snapshot& s1 = snaps[std::min(b, snaps.size() - 1)];
    const Snapshot& s0 = snaps[b == 0 ? 0 : b - 1];
    const float span = s1.hostTime - s0.hostTime;
    const float f = span > 1e-5f ? std::clamp((renderTime - s0.hostTime) / span, 0.0f, 1.0f) : 1.0f;
    for (size_t i = 0; i < clientTable.size(); ++i) {
      if (clientTable[i] == entt::null || i >= s0.poses.size() || i >= s1.poses.size()) continue;
      Pose p;
      p.p = glm::mix(s0.poses[i].p, s1.poses[i].p, f);
      p.r = glm::slerp(s0.poses[i].r, s1.poses[i].r, f);
      applyPose(world, scene, clientTable[i], p);
    }
    world.time = renderTime;
  }
};

Network::Network() : p_(std::make_unique<Impl>()) {}
Network::~Network() = default;
bool Network::isHost() const { return p_->isHost; }
bool Network::isClient() const { return p_->isClient; }
int Network::peers() const { return p_->isHost ? int(p_->slotOf.size()) : (p_->server && p_->welcomed ? 1 : 0); }

bool Network::host(World& world, uint16_t port) {
  if (enet_initialize() != 0) return false;
  ENetAddress addr{};
  addr.host = ENET_HOST_ANY;
  addr.port = port;
  const size_t slots = std::max<size_t>(world.slotActive.size(), 1);
  p_->host = enet_host_create(&addr, slots, 2, 0, 0);
  if (!p_->host) {
    LOG_ERROR("Multijogador: não consegui abrir a porta %u", port);
    return false;
  }
  p_->isHost = true;
  p_->table = replicated(world);
  if (world.slotActive.empty()) LOG_WARN("Multijogador: a cena não tem \"multijogador\" (nenhuma vaga para clientes)");
  LOG_INFO("Multijogador: anfitrião na porta %u, %zu vagas, %zu entidades replicadas", port, world.slotActive.size(),
           p_->table.size());
  return true;
}

bool Network::connect(World& world, const std::string& address, uint16_t port, float timeoutSeconds) {
  if (enet_initialize() != 0) return false;
  p_->host = enet_host_create(nullptr, 1, 2, 0, 0);
  if (!p_->host) return false;
  ENetAddress addr{};
  if (enet_address_set_host(&addr, address.c_str()) != 0) {
    LOG_ERROR("Multijogador: endereço inválido %s", address.c_str());
    return false;
  }
  addr.port = port;
  p_->server = enet_host_connect(p_->host, &addr, 2, 0);
  ENetEvent ev;
  if (!p_->server || enet_host_service(p_->host, &ev, uint32_t(timeoutSeconds * 1000.0f)) <= 0 ||
      ev.type != ENET_EVENT_TYPE_CONNECT) {
    LOG_ERROR("Multijogador: não consegui conectar em %s:%u", address.c_str(), port);
    if (p_->server) enet_peer_reset(p_->server);
    p_->server = nullptr;
    return false;
  }
  p_->isClient = true;
  world.netClient = true;
  LOG_INFO("Multijogador: conectado a %s:%u", address.c_str(), port);
  return true;
}

void Network::update(World& world, scene::Scene& scene, float dt, const PlayerInput& localInput) {
  if (p_->isHost) p_->hostUpdate(world, dt);
  else if (p_->isClient) p_->clientUpdate(world, scene, dt, localInput);
}

}  // namespace game
