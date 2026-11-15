#pragma once
#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include "editor/graph_editor.h"
#include "rhi/vk_common.h"

struct GLFWwindow;
namespace rhi {
struct Context;
}
namespace scene {
class Scene;
struct Camera;
}  // namespace scene
namespace game {
class World;
}

namespace editor {

// Editor visual sobre a janela do motor (Dear ImGui + ImGuizmo): painel da cena (jogar/pausar, salvar), lista de
// entidades, propriedades da selecionada, alças para mover e girar, e seleção clicando no objeto (raio da física).
// Desenhado por cima da imagem final, no mesmo passe de apresentação.
class Editor {
 public:
  // panels = falso: só a interface de jogo (HUD dos scripts), sem os painéis do editor.
  void init(rhi::Context& ctx, GLFWwindow* window, VkFormat format, uint32_t imageCount, bool panels = true);
  void shutdown();
  // Monta a interface do quadro (antes de ler o teclado e o mouse para a câmera).
  void frame(game::World* world, scene::Scene& scene, const scene::Camera& camera, VkExtent2D extent, float frameMs);
  // Desenha a interface sobre `target` (layout COLOR_ATTACHMENT_OPTIMAL).
  void render(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent);
  // A interface está usando o mouse / o teclado neste quadro (a câmera não deve reagir).
  bool capturingMouse() const;
  bool capturingKeyboard() const;
  bool ready() const { return ready_; }
  // Pedido de recarregar a cena (depois de criar, duplicar ou apagar): o arquivo temporário já foi gravado.
  bool consumeReload(std::string& path) {
    if (reload_.empty()) return false;
    path.swap(reload_);
    reload_.clear();
    selected_ = entt::null;
    return true;
  }
  // Seleciona a entidade com este nome (--select: testes e capturas de tela).
  void selectByName(game::World& world, const std::string& name);
  bool openGraph(const std::string& path) { return graph_.open(path); }  // --grafo

 private:
  void entityList(game::World& world);
  void properties(game::World& world, scene::Scene& scene);
  void gizmo(game::World& world, scene::Scene& scene, const scene::Camera& camera, VkExtent2D extent);
  void picking(game::World& world, const scene::Camera& camera, VkExtent2D extent);
  // Pincel de terreno: painel (modo, raio, força, salvar o mapa) e pinceladas com o botão esquerdo arrastado na cena.
  void terrainPanel(game::World& world, scene::Scene& scene, const scene::Camera& camera, VkExtent2D extent, float dt);
  bool brushRay(game::World& world, const scene::Camera& camera, VkExtent2D extent, glm::vec2 pixel, glm::vec3& hit);

  void drawHud(const game::World& world, VkExtent2D extent);

  bool ready_ = false;
  bool panels_ = true;
  VkFormat format_ = VK_FORMAT_UNDEFINED;
  entt::entity selected_ = entt::null;
  int gizmoOperation_ = 0;  // 0 = mover, 1 = girar
  bool playing_ = false;    // Jogar apertado (as poses de antes ficam guardadas para o Parar)
  bool editModeSet_ = false;
  char savePath_[256] = "cena_salva.json";
  bool brushOn_ = false;
  int brushMode_ = 0;  // subir, descer, aplanar, suavizar
  float brushRadius_ = 6.0f, brushStrength_ = 2.0f, brushLevel_ = 0.0f;
  char terrainPath_[256] = "terreno.altura";
  GraphEditor graph_;
  char graphPath_[256] = "script.vs.json";
  std::string status_;
  std::string reload_;
  void requestReload(game::World& world);
  void graphTest();
  bool contaOk();  // tela de login da conta Legacy enquanto não houver conta
  char loginEmail_[128] = {};
  char loginSenha_[128] = {};
  std::string loginErro_;
};

}  // namespace editor
