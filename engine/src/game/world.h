#pragma once
#include <memory>
#include <string>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "game/physics.h"

namespace scene {
class Scene;
struct Camera;
}  // namespace scene

namespace game {

// ---- Componentes (dados puros; os sistemas do World agem sobre eles) ----

struct Name {
  std::string value;
};

// Posição e orientação no mundo. A escala fica na forma (Shape), que define o tamanho do objeto.
struct Transform {
  glm::vec3 position{0.0f};
  glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

enum class ShapeType { Box, Sphere, Capsule };

// Forma do objeto: o mesmo tamanho vale para o desenho e para a colisão.
struct Shape {
  ShapeType type = ShapeType::Box;
  glm::vec3 halfExtents{0.5f};  // caixa
  float radius = 0.5f;          // esfera e cápsula
  float halfHeight = 0.5f;      // cápsula: metade do trecho reto (sem as tampas)
};

// Desenhado pelo renderizador: objeto da scene::Scene que acompanha o Transform.
struct Renderable {
  uint32_t sceneObject = ~0u;
  std::string material;  // nome no arquivo da cena (para salvar de volta)
};

// Simulado pela física. bodyId é preenchido em World::start().
struct RigidBody {
  BodyDesc desc;
  uint32_t bodyId = ~0u;
};

// ---- Mundo ----

// Camada de jogo sobre o renderizador: entidades com componentes (EnTT), física (Jolt) e a cena em arquivo (JSON).
// Cada quadro, update() avança a física e copia as poses para os objetos da scene::Scene.
class World {
 public:
  World();
  ~World();

  // Lê uma cena .json e cria as entidades, as malhas e os objetos de desenho (antes do upload da Scene), o sol e a
  // câmera. Formato e exemplo: cenas/fisica.json. Falso se o arquivo não abrir ou estiver inválido.
  bool load(const std::string& path, scene::Scene& scene, scene::Camera& camera);
  // Grava o estado atual (poses depois da simulação) no mesmo formato.
  bool save(const std::string& path) const;

  // Cria os corpos da física para as entidades com RigidBody.
  void start();
  // Avança a física `dt` segundos e move os objetos de desenho.
  void update(scene::Scene& scene, float dt);

  entt::registry registry;
  std::unique_ptr<Physics> physics;

 private:
  struct FileData;
  std::unique_ptr<FileData> file_;  // materiais, sol e câmera lidos (para salvar de volta)
};

// Cenas .json passam por aqui (scene::buildDemoScene): o mundo fica vivo enquanto a Scene existir e roda dentro do
// Scene::update. currentWorld() dá acesso a ele (ex.: --save-world no fim do modo sem janela).
bool loadWorldScene(const std::string& path, scene::Scene& scene, scene::Camera& camera);
World* currentWorld();

}  // namespace game
