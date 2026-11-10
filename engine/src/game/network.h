#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace scene {
class Scene;
}

namespace game {

class World;
struct PlayerInput;

// Multijogador por UDP (ENet), com o anfitrião autoritativo: ele simula o mundo inteiro (física, personagens, carros,
// scripts) e manda as poses de tudo que se mexe 30 vezes por segundo; cada cliente manda só a sua entrada (andar,
// pular, correr, direção da câmera) e mostra o estado recebido, interpolado 100 ms atrás (movimento liso mesmo com
// pacotes perdidos ou atrasados). Canal 0 confiável (boas-vindas: vaga e tabela de nomes), canal 1 não confiável
// (entrada e estado). Cada cliente ocupa uma vaga de jogador ("multijogador": {"vagas": N} na cena).
class Network {
 public:
  Network();
  ~Network();
  bool host(World& world, uint16_t port);
  bool connect(World& world, const std::string& address, uint16_t port, float timeoutSeconds = 60.0f);
  // A cada quadro, antes do World::update: recebe e manda pacotes; no cliente, aplica o estado interpolado.
  void update(World& world, scene::Scene& scene, float dt, const PlayerInput& localInput);
  bool isHost() const;
  bool isClient() const;
  int peers() const;  // clientes conectados (anfitrião) ou 1 se conectado (cliente)

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
