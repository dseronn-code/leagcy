#include "game/network.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <array>
#include <map>
#include <vector>

#ifdef _WIN32  // o enet.h inclui o windows.h: sem as macros min/max e o resto do Win32 que não usamos
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <enet/enet.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

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
struct WheelPose {
  glm::vec3 p, s;
  glm::quat r;
};
struct Snapshot {
  float hostTime;
  std::vector<Pose> poses;
  std::map<uint16_t, std::array<WheelPose, 4>> wheels;  // rodas dos carros (índice da tabela)
};

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

struct Network::Impl {
  ENetHost* host = nullptr;
  ENetPeer* server = nullptr;  // cliente: conexão com o anfitrião
  bool isHost = false, isClient = false;
  // Anfitrião
  std::map<ENetPeer*, int> slotOf;
  std::map<int, uint32_t> lastSeq;  // última entrada recebida de cada vaga (o cliente compara com a previsão)
  std::vector<entt::entity> table;  // ordem da tabela de nomes
  float sendTimer = 0.0f;
  // Cliente
  std::vector<entt::entity> clientTable;  // índice do anfitrião -> entidade local (null se não existe aqui)
  std::deque<Snapshot> snaps;
  float renderTime = -1.0f;
  bool welcomed = false;
  uint32_t seq = 0;                                       // número da última entrada enviada
  std::deque<std::pair<uint32_t, glm::vec3>> predicted;   // (entrada, pés previstos depois dela)
  // Atraso simulado (LEGACY_REDE_ATRASO_MS, só para testes): pacotes recebidos e enviados esperam este tempo.
  double delay = 0.0;
  std::deque<std::pair<double, std::vector<uint8_t>>> inbox, outbox;

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
        lastSeq[slot] = 0;
        world.activateSlot(slot, true);
        welcome(world, ev.peer, slot);
      } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        if (auto it = slotOf.find(ev.peer); it != slotOf.end()) {
          world.activateSlot(it->second, false);
          lastSeq.erase(it->second);
          slotOf.erase(it);
        }
      } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        Reader r{ev.packet->data, ev.packet->dataLength};
        if (r.get<uint8_t>() == kInput) {
          auto it = slotOf.find(ev.peer);
          const uint32_t seqIn = r.get<uint32_t>();
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
            lastSeq[it->second] = std::max(lastSeq[it->second], seqIn);
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
    // Confirmação da entrada de cada vaga (o cliente corrige a previsão) e as rodas dos carros.
    w.put(uint8_t(lastSeq.size()));
    for (const auto& [slot, sq] : lastSeq) {
      w.put(int8_t(slot));
      w.put(sq);
    }
    uint16_t nVeh = 0;
    for (entt::entity e : table) nVeh += world.registry.valid(e) && world.registry.all_of<Vehicle>(e) ? 1 : 0;
    w.put(nVeh);
    for (uint32_t i = 0; i < table.size(); ++i) {
      if (!world.registry.valid(table[i]) || !world.registry.all_of<Vehicle>(table[i]) || !world.scene) continue;
      const Vehicle& v = world.registry.get<Vehicle>(table[i]);
      w.put(uint16_t(i));
      for (uint32_t k = 0; k < 4; ++k) {
        glm::vec3 pos(0.0f), scale(1.0f), skew;
        glm::vec4 persp;
        glm::quat rot(1.0f, 0.0f, 0.0f, 0.0f);
        if (v.wheelObjects[k] != ~0u) glm::decompose(world.scene->objects[v.wheelObjects[k]].transform, scale, rot, pos, skew, persp);
        w.put(pos);
        w.put(scale);
        w.put(rot.w);
        w.put(rot.x);
        w.put(rot.y);
        w.put(rot.z);
      }
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

  entt::entity localEntity(const World& world) const { return world.slotEntity(world.localSlot); }

  void handlePacket(World& world, const std::vector<uint8_t>& data) {
    Reader r{data.data(), data.size()};
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
      uint32_t ack = 0;
      bool hasAck = false;
      const uint8_t nAck = r.get<uint8_t>();
      for (uint8_t k = 0; k < nAck && r.ok; ++k) {
        const int8_t slot = r.get<int8_t>();
        const uint32_t sq = r.get<uint32_t>();
        if (slot == world.localSlot) { ack = sq; hasAck = true; }
      }
      const uint16_t nVeh = r.get<uint16_t>();
      for (uint16_t k = 0; k < nVeh && r.ok; ++k) {
        const uint16_t i = r.get<uint16_t>();
        std::array<WheelPose, 4> wp;
        for (WheelPose& p : wp) {
          p.p = r.get<glm::vec3>();
          p.s = r.get<glm::vec3>();
          p.r.w = r.get<float>();
          p.r.x = r.get<float>();
          p.r.y = r.get<float>();
          p.r.z = r.get<float>();
        }
        if (r.ok) s.wheels[i] = wp;
      }
      if (!r.ok) return;
      if (hasAck) reconcile(world, s, ack);
      if (snaps.empty() || s.hostTime > snaps.back().hostTime) snaps.push_back(std::move(s));
      while (snaps.size() > 32) snaps.pop_front();
    }
  }

  // Correção da previsão: o anfitrião diz até qual entrada já processou e onde o boneco estava; a diferença para o
  // que este cliente previu naquela entrada é o erro. Aplicado aos poucos (sem tranco), ou de uma vez se for grande
  // (teleporte, nascimento). As previsões mais novas andam junto com a correção.
  void reconcile(World& world, const Snapshot& s, uint32_t ack) {
    if (!world.predictLocal) return;
    const entt::entity e = localEntity(world);
    if (e == entt::null) return;
    size_t ti = 0;
    while (ti < clientTable.size() && clientTable[ti] != e) ++ti;
    if (ti >= clientTable.size()) return;
    const Character& c = world.registry.get<Character>(e);
    const glm::vec3 serverFeet = s.poses[ti].p - glm::vec3(0.0f, 0.5f * c.desc.height, 0.0f);
    while (!predicted.empty() && predicted.front().first < ack) predicted.pop_front();
    glm::vec3 err;
    if (!predicted.empty() && predicted.front().first == ack) err = serverFeet - predicted.front().second;
    else if (predicted.empty()) err = serverFeet - world.physics->characterFeet(c.id);
    else return;
    const float len = glm::length(err);
    if (len < 1e-4f) return;
    const glm::vec3 fix = len > 2.0f ? err : err * 0.35f;
    world.physics->nudgeCharacter(c.id, fix);
    for (auto& [sq, feet] : predicted) feet += fix;
  }

  void clientUpdate(World& world, scene::Scene& scene, float dt, const PlayerInput& input) {
    const double now = nowSeconds();
    ENetEvent ev;
    while (enet_host_service(host, &ev, 0) > 0) {
      if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
        LOG_WARN("Multijogador: o anfitrião desconectou");
        server = nullptr;
      } else if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
        inbox.emplace_back(now + delay, std::vector<uint8_t>(ev.packet->data, ev.packet->data + ev.packet->dataLength));
        enet_packet_destroy(ev.packet);
      }
    }
    while (!inbox.empty() && inbox.front().first <= now) {
      handlePacket(world, inbox.front().second);
      inbox.pop_front();
    }
    if (server && welcomed) {  // a entrada do jogador, todo quadro, numerada
      if (seq > 0 && world.predictLocal) {  // pés previstos depois da entrada anterior (aplicada no quadro passado)
        const entt::entity e = localEntity(world);
        if (e != entt::null && world.registry.get<Character>(e).id != ~0u) {
          predicted.emplace_back(seq, world.physics->characterFeet(world.registry.get<Character>(e).id));
          while (predicted.size() > 600) predicted.pop_front();
        }
      }
      ++seq;
      Writer w;
      w.put(uint8_t(kInput));
      w.put(seq);
      w.put(input.move.x);
      w.put(input.move.y);
      w.put(input.cameraYaw);
      w.put(uint8_t((input.jump ? 1 : 0) | (input.run ? 2 : 0)));
      outbox.emplace_back(now + delay, std::move(w.b));
    }
    while (server && !outbox.empty() && outbox.front().first <= now) {
      Writer w;
      w.b = std::move(outbox.front().second);
      send(server, w, 1, false);
      outbox.pop_front();
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
    const entt::entity mine = world.predictLocal ? localEntity(world) : entt::null;
    for (size_t i = 0; i < clientTable.size(); ++i) {
      if (clientTable[i] == entt::null || clientTable[i] == mine || i >= s0.poses.size() || i >= s1.poses.size()) continue;
      Pose p;
      p.p = glm::mix(s0.poses[i].p, s1.poses[i].p, f);
      p.r = glm::slerp(s0.poses[i].r, s1.poses[i].r, f);
      applyPose(world, scene, clientTable[i], p);
    }
    for (const auto& [i, w1] : s1.wheels) {  // rodas: posição, giro e esterço vindos do anfitrião
      if (i >= clientTable.size() || clientTable[i] == entt::null || !world.registry.all_of<Vehicle>(clientTable[i])) continue;
      auto it0 = s0.wheels.find(i);
      const std::array<WheelPose, 4>& w0 = it0 != s0.wheels.end() ? it0->second : w1;
      const Vehicle& v = world.registry.get<Vehicle>(clientTable[i]);
      for (uint32_t k = 0; k < 4; ++k) {
        if (v.wheelObjects[k] == ~0u) continue;
        const glm::vec3 p = glm::mix(w0[k].p, w1[k].p, f), sc = glm::mix(w0[k].s, w1[k].s, f);
        const glm::quat q = glm::slerp(w0[k].r, w1[k].r, f);
        scene.setTransform(v.wheelObjects[k], glm::translate(glm::mat4(1.0f), p) * glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), sc));
      }
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
  if (const char* d = std::getenv("LEGACY_REDE_ATRASO_MS")) p_->delay = std::atof(d) / 1000.0;  // testes
  if (std::getenv("LEGACY_REDE_SEM_PREVISAO")) world.predictLocal = false;
  LOG_INFO("Multijogador: conectado a %s:%u", address.c_str(), port);
  return true;
}

void Network::update(World& world, scene::Scene& scene, float dt, const PlayerInput& localInput) {
  if (p_->isHost) p_->hostUpdate(world, dt);
  else if (p_->isClient) p_->clientUpdate(world, scene, dt, localInput);
}

}  // namespace game
