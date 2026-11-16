#pragma once
#include <filesystem>
#include <set>

#include <nlohmann/json_fwd.hpp>
#include <array>
#include <functional>
#include <memory>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "game/audio.h"
#include "game/input.h"
#include "game/physics.h"
#include "scene/mesh.h"

namespace scene {
class Scene;
struct Camera;
struct Model;
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

// Modelo glTF (cenário, prédios): os objetos que ele criou na scene::Scene. Fica onde foi posto (parado).
struct Model {
  std::string path;
  float scale = 1.0f;
  uint32_t firstObject = 0, objectCount = 0;
  bool meshCollider = false;  // colisão pelos triângulos do modelo
  float friction = 0.6f;
  uint32_t bodyId = ~0u;
};

// Som preso à entidade (segue a posição dela). handle é preenchido em World::start().
struct SoundSource {
  std::string path;
  Audio::SoundDesc desc;
  bool autoplay = true;
  uint32_t handle = ~0u;
};

// Comportamento em Lua (src/game/scripting.h): o arquivo roda num ambiente próprio da entidade.
struct Script {
  std::string path;
};

// Simulado pela física. bodyId é preenchido em World::start().
struct RigidBody {
  BodyDesc desc;
  uint32_t bodyId = ~0u;
};

// Um passo do roteiro de um personagem controlado por arquivo (testes, NPCs simples): a partir de `time` segundos,
// anda na direção `move` (mundo, normalizada) e pula uma vez se `jump`.
struct ScriptKey {
  float time = 0.0f;
  glm::vec3 move{0.0f};
  bool jump = false, run = false;
};

// Personagem: anda, corre, pula. Controle pelo jogador (teclado e mouse na janela) ou por roteiro.
// Aparência animada de um personagem ("visual": modelo glTF no lugar da cápsula): mistura parado/andar/correr pela
// velocidade (passos sincronizados), ações por script com transição suave e IK dos pés no chão de verdade.
struct CharacterVisual {
  bool enabled = false;
  std::string path;
  float scale = 1.0f;
  glm::vec3 offset{0.0f};  // do pé da cápsula até a origem do modelo (no espaço do personagem)
  float yawDeg = 0.0f;     // giro para a frente do modelo apontar para +Z
  bool autoOffset = false;  // calcular offset.y para os pés da pose parada ficarem no chão
  std::string idle, walk, run;
  float walkRef = 1.5f, runRef = 4.5f;  // m/s em que os clipes andar/correr foram feitos (sem patinar)
  float fade = 0.2f;                    // s de transição
  bool ik = true;
  std::vector<std::array<std::string, 3>> legs;  // quadril, joelho, tornozelo
  // Retargeting: as animações de outro modelo (outro esqueleto) copiadas para este, pelo mapa de ossos (origem ->
  // destino) ou pelos nomes iguais tirando os prefixos de cada lado.
  std::string retargetFrom, retargetSrcPrefix, retargetDstPrefix;
  std::vector<std::pair<std::string, std::string>> retargetMap;
  // Cabelo por fios ("cabelo_fios"): calota de fios simulados presa a um osso (a cabeça), no lugar das cartelas.
  std::string hairBone;
  uint32_t hairStrands = 0, hairSegments = 12, hairSeed = 3;
  float hairLength = 0.6f, hairHalfWidth = 0.0011f;
  glm::vec3 hairCenter{0.0f}, hairRadius{0.1f}, hairColor{0.08f, 0.06f, 0.05f};
  int hairNode = -1, hairObject = -1;
  // Rosto (morph targets do modelo): pesos pedidos pelo script (e:expressao) e o piscar automático.
  std::map<std::string, float> expressions;
  float blinkTimer = 2.0f, blinkTime = -1.0f;
  // Estado.
  int model = -1, idleA = -1, walkA = -1, runA = -1;
  float speed = 0.0f, yaw = 0.0f, phase = 0.0f, idleTime = 0.0f;
  int action = -1;
  bool actionOn = false, actionLoop = false;
  float actionTime = 0.0f, actionWeight = 0.0f, actionFade = 0.2f;
};

struct Character {
  CharacterDesc desc;
  CharacterVisual visual;
  float walkSpeed = 4.5f, runSpeed = 8.0f, jumpSpeed = 5.0f;  // m/s
  bool player = false;
  std::vector<ScriptKey> script;
  size_t scriptJumped = 0;  // quantos passos do roteiro já tiveram o pulo atendido
  uint32_t id = ~0u;        // na Physics
  // IA ("controle": "ia"): anda pela malha de navegação até um ponto ou seguindo outra entidade.
  int remoteSlot = -1;  // multijogador: vaga de jogador remoto (entrada vem da rede)
  glm::vec3 netVelocity{0.0f};  // cliente de rede: velocidade estimada pelas poses recebidas (anima o "visual")
  glm::vec3 netLastPos{0.0f};
  bool netHasLast = false;
  bool ai = false;
  bool aiHasTarget = false;
  glm::vec3 aiTarget{0.0f};
  std::string aiFollow;      // nome da entidade seguida (vazio = ponto fixo)
  float aiStopDistance = 0.5f;
  std::vector<glm::vec3> aiPath;
  size_t aiNext = 0;
  float aiRepath = 0.0f;     // s até recalcular o caminho
  // Comportamentos ("ia": {"patrulha", "perseguir", "fugir_de", "visao", "campo_visao"}): patrulha os pontos; quem
  // está em "perseguir" é perseguido enquanto visto (distância, cone de visão e linha de visada) e depois procurado no
  // último lugar visto; de quem está em "fugir_de" foge quando vê.
  std::vector<glm::vec3> aiPatrol;
  size_t aiPatrolIdx = 0;
  std::string aiChase, aiFlee;
  float aiSight = 12.0f, aiFovDeg = 140.0f;
  glm::vec3 aiFacing{0.0f, 0.0f, 1.0f};
  glm::vec3 aiLastSeen{0.0f};
  float aiSearchTime = 0.0f;
  float aiFleeTime = 0.0f;  // s que ainda foge depois de perder a ameaça de vista
  glm::vec3 aiThreat{0.0f};  // onde a ameaça foi vista por último
  float aiHearing = 2.5f;    // m: perto assim percebe mesmo fora do cone de visão (ouve)
  glm::vec3 aiGoal{0.0f};
  bool aiHasGoal = false;
  std::string aiState = "parado";  // parado, patrulhando, perseguindo, procurando, fugindo, indo, seguindo
  // Ragdoll (e:ragdoll): o personagem vira corpos ligados por juntas e o "visual" segue a física; e:levantar volta.
  struct Ragdoll {
    bool active = false;
    std::vector<uint32_t> bodies, nodes;  // corpo da física e nó do modelo de cada parte
    std::vector<int> parents;             // parte mãe de cada parte (-1 = a bacia)
    std::vector<glm::mat4> offset;        // do corpo até o nó (fixo, da pose em que caiu)
    glm::mat4 rootBasis{1.0f};            // rotação e escala do modelo (sem a posição)
    glm::vec3 rootToPelvis{0.0f};         // da origem do modelo até a bacia, no mundo, na pose em que caiu
    float blendOut = 0.0f;                // s que faltam da volta para a animação depois de levantar
    float strength = 0.0f;                // ragdoll ativo: força dos motores das juntas (0 = solto)
    bool followAnimation = false;         // alvo: a animação tocando (senão, a pose de quando caiu)
    std::vector<glm::quat> fallRotation;  // rotação de cada parte na hora da queda
    bool motorsOn = false;
  } ragdoll;
};

// Um passo do roteiro de um veículo (testes, carros de IA simples): a partir de `time` s, pedais e direção.
struct DriveKey {
  float time = 0.0f;
  float throttle = 0.0f, steer = 0.0f, brake = 0.0f, handBrake = 0.0f;
};

// Veículo de 4 rodas. O chassi é desenhado pelo Renderable da entidade; as rodas são 4 objetos a mais.
struct Vehicle {
  VehicleDesc desc;
  bool player = false;
  std::vector<DriveKey> script;
  uint32_t id = ~0u;  // na Physics
  uint32_t wheelObjects[4] = {~0u, ~0u, ~0u, ~0u};
  std::string wheelMaterial;
  // Carro tirado do trânsito: as peças do modelo glTF (chão em y = 0, frente para +z) seguem o chassi; sem caixa nem
  // cilindros. `parked`: o jogador saiu, freio de mão puxado.
  std::vector<uint32_t> visual;
  bool parked = false;
  // Viatura perseguindo o jogador (World::updatePolice): dirige sozinha até `aiTarget`; giroflex piscando.
  bool police = false, chasing = false;
  glm::vec3 aiTarget{0.0f};
  float stuck = 0.0f, reverse = 0.0f;
  uint32_t sirenRed = ~0u, sirenBlue = ~0u;
};

// Um carro que o trânsito entrega para o jogador dirigir (World::grabTraffic).
struct TrafficGrab {
  std::vector<uint32_t> objects;  // os objetos do carro na cena (passam a seguir o chassi; objeto novo no meio do
                                 // jogo não entra no renderizador)
  glm::vec2 position{0.0f};
  float yaw = 0.0f, length = 4.8f, speed = 0.0f;
  bool bus = false, police = false;
  uint32_t sirenRed = ~0u, sirenBlue = ~0u;
};

// Entrada do jogador neste quadro: movimento relativo à câmera (x = direita, y = frente, -1..1) e a direção da câmera.
struct PlayerInput {
  glm::vec2 move{0.0f};
  float cameraYaw = 0.0f;  // radianos (scene::Camera::yaw)
  bool jump = false, run = false;
};

// Interface de jogo (HUD): itens desenhados por cima da imagem, montados pelos scripts a cada quadro de jogo.
// Posição e tamanho em fração da tela (0..1, origem no canto de cima à esquerda); texto com altura em fração da
// altura da tela. Cores RGBA sRGB de 0 a 1, como num editor de imagem (a = opacidade).
struct HudItem {
  enum class Kind { Text, Rect, Bar } kind = Kind::Text;
  glm::vec2 pos{0.0f}, size{0.0f};
  float value = 0.0f;   // barra: fração cheia (0..1)
  float textSize = 0.04f;
  int align = 0;        // texto: 0 = esquerda, 1 = centro, 2 = direita (em relação a pos.x)
  glm::vec4 color{1.0f}, back{0.0f, 0.0f, 0.0f, 0.5f};
  std::string text;
};

// Estado dos controles de interface (hud.botao, hud.deslizar, hud.alternar): foco para teclado e controle.
struct UiState {
  int focus = 0;       // controle com foco (ordem de criação no quadro)
  int count = 0;       // controles criados neste quadro
  int prevCount = 0;   // ... e no anterior (navegação com setas / direcional)
  int dragging = -1;   // controle deslizante sendo arrastado pelo mouse
  int hoverFocus = -1; // controle sob o mouse (que se mexeu) no quadro anterior: vira o foco no começo do quadro
  glm::vec2 lastMouse{-1.0f};
  bool mouseMoved = false;
};
// Partículas presas a uma entidade ("particulas": fogo, fumaça ou faíscas): o emissor acompanha a posição dela e o
// script liga e desliga (e:particulas(sim)).
struct ParticleSource {
  uint32_t emitter = ~0u;
  std::string type;
  glm::vec3 offset{0.0f};
  float radius = 0.3f, scale = 1.0f;
  uint32_t count = 400;
  bool enabled = true;
};

// Marca entidades do sistema (o menu padrão): ficam fora do arquivo salvo e do editor.
struct SystemEntity {};
// Mundo aberto ("mundo_aberto" na cena): entidade de uma célula longe do jogador — sem desenho, física nem script até
// a célula voltar. "sempre_carregado": true na entidade a deixa fora das células.
struct Dormant {};
struct AlwaysLoaded {};
// "no_chao": a altura da entidade (e da patrulha da IA) conta a partir do terreno, posta no começo do jogo.
struct OnGround {};
// Ladrilho do terreno ("terreno" na cena). De perto, a malha detalhada (o Renderable) com colisão (bodyId); com a
// célula dormindo, só a malha simples (farObject), sem física.
struct TerrainTile {
  glm::vec2 center{0.0f};  // centro do ladrilho (x, z)
  uint32_t farObject = ~0u;
  uint32_t bodyId = ~0u;
  float friction = 0.8f;
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

  // Editor: entidade dona de um corpo da física (entt::null se nenhuma) e aplicar uma pose nova (corpo + desenho).
  entt::entity entityOfBody(uint32_t body) const;
  void setEntityPose(scene::Scene& scene, entt::entity e, glm::vec3 position, glm::quat rotation);
  // Matriz de desenho de uma entidade (posição, rotação e tamanho da forma).
  glm::mat4 entityMatrix(entt::entity e) const;
  bool simulate = true;  // falso: física parada (editor em pausa)
  // Ação de animação num personagem com "visual" (e:animar): clipe pelo nome, transição em s, em laço ou não.
  bool playAction(entt::entity e, const std::string& clip, float fade, bool loop);
  void stopAction(entt::entity e, float fade);
  // Posição no mundo de um osso (nó do glTF) do modelo de um personagem com "visual"; falso se não existe.
  bool bonePosition(entt::entity e, const std::string& bone, glm::vec3& out) const;
  // Ragdoll num personagem com "visual": corpos por osso a partir da bacia, com a velocidade atual mais
  // impulse / massa. Falso se não houver esqueleto. stopRagdoll põe o personagem de pé onde a bacia caiu.
  static constexpr float kRagdollBlendOut = 0.5f;  // s da pose caída até a animação ao levantar
  bool startRagdoll(entt::entity e, glm::vec3 impulse);
  // Ragdoll ativo (no estilo do Physical Animation): motores nas juntas puxam cada parte para a pose da animação.
  bool setRagdollStrength(entt::entity e, float strength, bool followAnimation = false);
  float ragdollDeviation(entt::entity e) const;
  std::vector<glm::quat> ragdollTargets(const scene::Model& m, const Character::Ragdoll& r) const;  // graus: média, nas juntas, entre a pose da física e a da animação
  bool stopRagdoll(entt::entity e);
  scene::Scene* scene = nullptr;  // a cena deste mundo (consultas dos scripts: ossos, animação)
  std::vector<HudItem> hud;  // interface de jogo deste quadro (refeita pelos scripts enquanto o jogo roda)
  GameInput gameInput;       // teclado, mouse e controle neste quadro (preenchido pelo main)
  UiState ui;
  // Pausa do JOGO (pausar() no script, menu de pausa): física, personagens e o tempo de jogo param; os scripts
  // continuam rodando (o menu). Diferente de `simulate` (o editor parado, sem scripts).
  bool paused = false;
  bool quitRequested = false;        // sair() no script
  // Idiomas (t("texto") no script): idiomas/<código>.json com {"_nome": "English", "Continuar": "Continue"}; a chave
  // é o texto original em português, então sem tradução (ou sem a pasta) aparece o português. Ordem de escolha:
  // --idioma, a escolha guardada do jogador (saves/idioma.txt), "idioma" da cena, "pt".
  std::string language = "pt";
  std::string translate(const std::string& key) const;
  bool setLanguage(const std::string& code, bool remember);
  std::vector<std::pair<std::string, std::string>> languages() const;  // (código, nome), em ordem de código
  std::string scenePath;
  // Rede (main): roda no começo do update, dentro do Scene::update, para as poses recebidas contarem como movimento
  // deste quadro (vetores de movimento certos no TAA e no motion blur).
  std::function<void(float)> networkStep;
  bool playerControl = true;         // controle_jogador(falso): o jogador para de responder (cutscene)
  // Entrar e sair de carros (tecla F / botão Y): o veículo mais perto (até 3,5 m), da cena ou do trânsito; o
  // personagem fica guardado enquanto dirige. `grabTraffic` é montado pelo "transito" da cena.
  std::function<bool(glm::vec2, float, TrafficGrab*)> grabTraffic;  // nullptr: só confere se há carro perto
  entt::entity driving = entt::null;
  void updateCarEntry(scene::Scene& scene);
  // Polícia (nível de procurado e o que ela sabe, como o Heat): crimes vistos sobem as estrelas; viaturas a até 45 m
  // veem o jogador (aparência a pé, o carro dirigindo); longe e fora da vista por um tempo, as estrelas caem (mais
  // rápido se ele trocou o carro que a polícia conhece). Com estrela, as viaturas do trânsito mais perto ligam o
  // giroflex e perseguem; colada no jogador a pé por 2,5 s, é preso.
  struct Heat {
    float level = 0.0f;                   // 0..6 (estrelas)
    glm::vec2 lastSeen{0.0f};
    float unseen = 0.0f, escape = 0.0f, arrest = 0.0f, messageTime = 0.0f, cooldown = 0.0f;
    entt::entity knownCar = entt::null;   // o carro em que a polícia viu o jogador
    bool knowsLook = false;               // viram o jogador a pé
    std::string message;
  } heat;
  void reportCrime(glm::vec2 where, float weight);
  void updatePolice(scene::Scene& scene, float dt);
  std::function<int(glm::vec2, int, std::vector<TrafficGrab>&)> grabPolice;  // até n viaturas do trânsito, as mais perto
  std::function<void(std::vector<glm::vec2>&)> policeInTraffic;             // onde estão as viaturas do trânsito
  std::function<void(bool)> sirensInTraffic;                                // giroflex das viaturas do trânsito
  float realTime = 0.0f;             // s desde o início, mesmo pausado (tempo_real())
  std::string qualityRequest;        // qualidade("medio"): o main recria o renderizador com o preset
  // Câmera de cinema pelo script (camera(x, y, z, ax, ay, az) / camera_jogo()): com `active`, o main põe a câmera em
  // `position` olhando para `target` (e o FOV, se > 0) no lugar da câmera do jogador.
  struct CameraOverride {
    bool active = false;
    glm::vec3 position{0.0f}, target{0.0f, 0.0f, -1.0f};
    float fovDeg = 0.0f;
  } cameraOverride;
  std::vector<std::pair<std::string, float>> optionRequests;  // opcao("motion_blur", 1): aplicadas pelo main
  // Salvar e carregar o jogo (saves/<nome>.json): poses e velocidades das entidades com nome que se movem, o tempo de
  // jogo e os dados do script (JSON). Carregar aplica no mundo atual (a mesma cena) e devolve os dados.
  // Explosão de um tiro (explosao() no script): bola de fogo, fragmentos, fumaça e o clarão de luz. Usa um dos
  // emissores reservados na carga (só quando algum script da cena chama explosao).
  bool explode(glm::vec3 position, float scale);
  std::vector<uint32_t> explosionSlots;
  size_t nextExplosion = 0;
  // Multijogador ("multijogador": {"vagas": N} na cena): o jogador é copiado em N vagas estacionadas fora do mapa;
  // o anfitrião ativa uma vaga quando alguém conecta (no ponto "nascer") e move o personagem dela pela entrada que
  // chega pela rede. No cliente (netClient) a física não roda: as poses vêm do anfitrião.
  std::vector<PlayerInput> remoteInputs;
  std::vector<bool> slotActive;
  std::vector<glm::vec3> slotSpawn;
  bool netClient = false;
  int localSlot = -1;  // cliente: a vaga deste jogador (a câmera segue ela)
  bool predictLocal = true;  // cliente: o próprio boneco anda na hora (previsão), o anfitrião corrige
  const PlayerInput& playerInput() const { return input_; }
  void activateSlot(int slot, bool on);
  entt::entity slotEntity(int slot) const;
  bool saveGame(const std::string& name, const std::string& dataJson) const;
  bool loadGame(const std::string& name, std::string& dataJson);
  static bool gameExists(const std::string& name);
  // Editor: cria uma forma básica (aparece no próximo load) e duplica uma entidade (1 m ao lado).
  entt::entity addPrimitive(ShapeType type, glm::vec3 position, Motion motion, glm::vec3 halfExtents = glm::vec3(0.5f));
  entt::entity duplicate(entt::entity e);
  // Editor: guarda as poses de todas as entidades ("Jogar") e volta para elas ("Parar"), com velocidades zeradas.
  void snapshot();
  void restore(scene::Scene& scene);

  // Jogador (personagem com "controle": "jogador"): entrada do quadro e posição dos pés para a câmera seguir.
  void setPlayerInput(const PlayerInput& input) { input_ = input; }
  bool playerFeet(glm::vec3& feet) const;
  // Carro do jogador ("controle": "jogador"): posição e frente para a câmera de perseguição.
  bool playerVehicle(glm::vec3& position, glm::vec3& forward) const;

  entt::registry registry;
  std::unique_ptr<Physics> physics;
  std::unique_ptr<class Scripting> scripting;  // criado em start() se houver entidades com Script
  std::unique_ptr<Audio> audio;                 // criado em start()
  std::unique_ptr<class Navigation> navigation; // criada em start() se houver personagem com IA
  // Ouvinte do áudio (a câmera), a cada quadro antes do update.
  void setListener(glm::vec3 position, glm::vec3 forward) {
    if (audio) audio->setListener(position, forward);
    listenerPos_ = position;
    hasListener_ = true;
  }
  // Mundo aberto (no estilo do World Partition): as entidades paradas com desenho (cenário, adereços, colisões) ficam
  // em células quadradas de `cellSize` m pela posição; a célula carrega quando o jogador (ou a câmera, sem jogador) chega
  // a `loadDistance` m dela e descarrega além de `unloadDistance` (folga: não fica piscando na borda). Personagens,
  // veículos, corpos dinâmicos, entidades só de lógica (sem desenho), o que é maior que uma célula e o que tem
  // "sempre_carregado" ficam sempre.
  struct OpenWorldCell {
    glm::vec2 center{0.0f};
    std::vector<entt::entity> members;
    bool loaded = true;
  };
  struct OpenWorld {
    bool enabled = false;
    float cellSize = 64.0f, loadDistance = 200.0f, unloadDistance = 260.0f;
    std::vector<OpenWorldCell> cells;
    uint32_t loaded = 0, entities = 0;
  } openWorld;
  void setEntityDormant(scene::Scene& scene, entt::entity e, bool dormant);
  float time = 0.0f;  // segundos de jogo simulados

 private:
  std::unordered_map<std::string, std::unordered_map<std::string, std::string>> texts_;  // idioma -> chave -> texto
  std::unordered_map<std::string, std::string> languageNames_;
  void loadLanguages();
 public:
  // Script visual (visual_script.h): gera o Lua e devolve o caminho dele (vazio se o grafo é inválido).
  std::string compileVisualSource(const nlohmann::json& src, const std::string& sceneDir);
  void refreshVisualScripts();  // chamado pela recarga a quente dos scripts: .json mudou = Lua novo
 private:
  struct VisualSource {
    std::string source, lua;
    std::filesystem::file_time_type stamp;
  };
  std::vector<VisualSource> visualSources_;
  struct FileData;
  std::unique_ptr<FileData> file_;  // materiais, sol e câmera lidos (para salvar de volta)
  PlayerInput input_;
  void animateCharacter(scene::Scene& scene, Character& c, glm::vec3 feet, float dt);
  void ragdollBlendStep(scene::Scene& scene, Character& c, float dt);
  void retargetCharacter(scene::Scene& scene, int model, const CharacterVisual& v);  // levantando: a pose caída some aos poucos
 public:
  // IA: o personagem `c` vê a entidade `other` (distância, cone de visão para onde ele olha e linha de visada)?
  bool sees(const Character& c, entt::entity other) const;
  entt::entity findByName(const std::string& name) const;
 private:
  glm::vec3 listenerPos_{0.0f};
  bool hasListener_ = false;
  void partitionOpenWorld();
 public:
  // Terreno de quilômetros ("terreno" na cena): altura por ruído fractal ou por um mapa de altura (PNG em tons de
  // cinza, 8 ou 16 bits), dividido em ladrilhos. 0 fora do terreno ou sem terreno.
  struct Terrain {
    bool enabled = false;
    float size = 2048.0f, tile = 64.0f, height = 80.0f, base = 0.0f, noiseScale = 512.0f;
    int octaves = 6;
    float seed = 0.0f;
    glm::vec2 center{0.0f};
    std::vector<float> map;  // 0..1, mapW x mapH (vazio: ruído); editável: altura em metros (base 0, altura 1)
    int mapW = 0, mapH = 0;
    uint32_t res = 64, farRes = 8;
    bool editable = false;  // o pincel já converteu a altura numa grade
  } terrain;
  float terrainHeight(float x, float z) const;
  scene::MeshData terrainTileMesh(float cx, float cz, uint32_t res, float skirt) const;
  // Pincel: "subir" / "descer" (strength em m no centro), "aplanar" (strength = altura alvo), "suavizar" (0..1).
  bool terrainBrush(scene::Scene& scene, glm::vec2 at, float radius, float strength, const std::string& mode);
  bool saveTerrainMap(const std::string& path) const;
  std::set<std::string> timelineRequests;  // tocar_linha() pede, o script da linha (linha_pedida) atende

 private:
  void updateOpenWorld(scene::Scene& scene);
  std::vector<std::pair<entt::entity, Transform>> snapshot_;
  float snapshotTime_ = 0.0f;
  scene::Scene* loadedScene_ = nullptr;  // para ler os triângulos dos modelos em start() (antes do upload)
};

// Cenas .json passam por aqui (scene::buildDemoScene): o mundo fica vivo enquanto a Scene existir e roda dentro do
// Scene::update. currentWorld() dá acesso a ele (ex.: --save-world no fim do modo sem janela).
bool loadWorldScene(const std::string& path, scene::Scene& scene, scene::Camera& camera);
// Áudio sem placa de som (modo sem janela): a mixagem fica na memória para --audio-wav. Antes de carregar a cena.
void setAudioOffline(bool offline);
// --idioma: vale acima da escolha guardada e da cena. Antes de carregar a cena.
void setLanguageOverride(const std::string& code);
World* currentWorld();

}  // namespace game
