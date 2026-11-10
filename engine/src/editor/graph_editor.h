#pragma once
// Editor de grafos do editor (Dear ImGui, sem biblioteca a mais): abre um script visual (.vs.json: "nos" + "ligacoes")
// ou um material por nós ("nos" + "saida"), mostra os nós como caixas arrastáveis com os campos editáveis e os fios
// entre os pinos, e grava o JSON de volta. Script visual: pinos de execução (saídas "proximo", "sim"/"nao",
// "corpo"/"depois"); material: cada campo com {"no": id} é um pino de entrada de dados. Salvar um .vs.json com o jogo
// rodando já recarrega o script (recarga a quente).
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

namespace editor {

class GraphEditor {
 public:
  bool open(const std::string& path);
  bool save();
  void draw();  // janela "Grafo" (só se algo estiver aberto)
  bool isOpen() const { return !path_.empty(); }

  // Operações (as mesmas da interface; usadas pelos testes sem mouse).
  int addNode(const std::string& type, glm::vec2 pos);
  bool connect(int from, const std::string& pin, int to);  // script: saída -> nó; material: campo `pin` de `to` <- from
  void removeNode(int id);
  const nlohmann::json& graph() const { return graph_; }

 private:
  bool material() const { return graph_.contains("saida"); }
  std::vector<std::string> outputs(const nlohmann::json& node) const;
  void layout();  // posições iniciais (colunas pela distância até um evento)
  nlohmann::json* node(int id);

  std::string path_, status_;
  nlohmann::json graph_;
  glm::vec2 scroll_{40.0f, 40.0f};
  int selected_ = -1;
  // Fio sendo puxado: nó e saída de origem (script) ou nó de origem (material).
  int dragFrom_ = -1;
  std::string dragPin_;
  glm::vec2 popupPos_{0.0f};
};

}  // namespace editor
