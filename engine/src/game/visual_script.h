#pragma once
// Script visual (no estilo do Blueprint): um grafo de nós em JSON — eventos ("ao_iniciar", "ao_tecla", "a_cada",
// "ao_chegar_perto"...) ligados a ações ("mover", "esperar", "som", "se", "repetir"...) — compilado para Lua e rodado
// pelo mesmo motor de scripts (recarga a quente inclusa). Ações que levam tempo ("esperar", "mover" com duração) rodam
// numa corrotina por disparo do evento, sem travar o jogo. Formato em docs/motor_completo.md.
#include <string>

#include <nlohmann/json.hpp>

namespace game {

// Gera o Lua do grafo. Devolve vazio e preenche `error` se o grafo é inválido (nó desconhecido, ligação para nó que não
// existe, ciclo fora de "repetir").
std::string compileVisualScript(const nlohmann::json& graph, std::string& error);

}  // namespace game
