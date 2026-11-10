#pragma once
#include <memory>
#include <string>

#include <entt/entt.hpp>

namespace scene {
class Scene;
}

namespace game {

class World;

// Scripts em Lua 5.4 (sol2) presos a entidades ("script": "scripts/x.lua" na cena). Cada entidade roda o arquivo no
// seu próprio ambiente (as variáveis globais do script são dela) e o motor chama:
//   iniciar(e)          uma vez, na primeira atualização
//   atualizar(e, dt)    todo quadro de jogo (dt em segundos)
// A API está em docs/motor_completo.md (Entidade: posicao, mover_para, girar, impulso, velocidade...; globais:
// achar, tempo, log). Arquivo alterado no disco = recarregado na hora, mantendo as variáveis da entidade.
// Erro num script: registrado uma vez e aquele script fica parado até o arquivo mudar (o jogo não cai).
class Scripting {
 public:
  explicit Scripting(World& world);
  ~Scripting();
  void attach(entt::entity e, const std::string& path);
  void update(scene::Scene& scene, float dt);
  int failures() const;  // scripts parados por erro

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace game
