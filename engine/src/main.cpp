// Legacy Engine — ponto de entrada.
//   legacy                      janela interativa (WASD + botão direito do mouse)
//   legacy --headless --frames 32 --out frame.png
//   legacy --scene outdoor --night --lights 1000
#include <cctype>
#include <algorithm>
#include <array>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "editor/editor.h"
#include "game/network.h"
#include "game/world.h"
#include "renderer/renderer.h"
#include "rhi/swapchain.h"
#include "scene/demo_scene.h"
#include "util/png.h"

namespace {

// ---- Entrada do jogo (teclado, mouse, controle) ----
const std::pair<const char*, int> kGameKeys[] = {
    {"a", GLFW_KEY_A}, {"b", GLFW_KEY_B}, {"c", GLFW_KEY_C}, {"d", GLFW_KEY_D}, {"e", GLFW_KEY_E}, {"f", GLFW_KEY_F},
    {"g", GLFW_KEY_G}, {"h", GLFW_KEY_H}, {"i", GLFW_KEY_I}, {"j", GLFW_KEY_J}, {"k", GLFW_KEY_K}, {"l", GLFW_KEY_L},
    {"m", GLFW_KEY_M}, {"n", GLFW_KEY_N}, {"o", GLFW_KEY_O}, {"p", GLFW_KEY_P}, {"q", GLFW_KEY_Q}, {"r", GLFW_KEY_R},
    {"s", GLFW_KEY_S}, {"t", GLFW_KEY_T}, {"u", GLFW_KEY_U}, {"v", GLFW_KEY_V}, {"w", GLFW_KEY_W}, {"x", GLFW_KEY_X},
    {"y", GLFW_KEY_Y}, {"z", GLFW_KEY_Z}, {"0", GLFW_KEY_0}, {"1", GLFW_KEY_1}, {"2", GLFW_KEY_2}, {"3", GLFW_KEY_3},
    {"4", GLFW_KEY_4}, {"5", GLFW_KEY_5}, {"6", GLFW_KEY_6}, {"7", GLFW_KEY_7}, {"8", GLFW_KEY_8}, {"9", GLFW_KEY_9},
    {"espaco", GLFW_KEY_SPACE}, {"esc", GLFW_KEY_ESCAPE}, {"enter", GLFW_KEY_ENTER}, {"enter", GLFW_KEY_KP_ENTER},
    {"tab", GLFW_KEY_TAB}, {"shift", GLFW_KEY_LEFT_SHIFT}, {"shift", GLFW_KEY_RIGHT_SHIFT},
    {"ctrl", GLFW_KEY_LEFT_CONTROL}, {"ctrl", GLFW_KEY_RIGHT_CONTROL}, {"alt", GLFW_KEY_LEFT_ALT},
    {"backspace", GLFW_KEY_BACKSPACE}, {"cima", GLFW_KEY_UP}, {"baixo", GLFW_KEY_DOWN}, {"esquerda", GLFW_KEY_LEFT},
    {"direita", GLFW_KEY_RIGHT}, {"f1", GLFW_KEY_F1}, {"f2", GLFW_KEY_F2}, {"f3", GLFW_KEY_F3}, {"f4", GLFW_KEY_F4},
    {"f5", GLFW_KEY_F5}, {"f6", GLFW_KEY_F6}, {"f7", GLFW_KEY_F7}, {"f8", GLFW_KEY_F8}, {"f9", GLFW_KEY_F9},
    {"f10", GLFW_KEY_F10}, {"f11", GLFW_KEY_F11}, {"f12", GLFW_KEY_F12}};
const std::pair<const char*, int> kPadButtons[] = {
    {"pad_a", GLFW_GAMEPAD_BUTTON_A}, {"pad_b", GLFW_GAMEPAD_BUTTON_B}, {"pad_x", GLFW_GAMEPAD_BUTTON_X},
    {"pad_y", GLFW_GAMEPAD_BUTTON_Y}, {"pad_lb", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER},
    {"pad_rb", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER}, {"pad_back", GLFW_GAMEPAD_BUTTON_BACK},
    {"pad_start", GLFW_GAMEPAD_BUTTON_START}, {"pad_l3", GLFW_GAMEPAD_BUTTON_LEFT_THUMB},
    {"pad_r3", GLFW_GAMEPAD_BUTTON_RIGHT_THUMB}, {"pad_cima", GLFW_GAMEPAD_BUTTON_DPAD_UP},
    {"pad_baixo", GLFW_GAMEPAD_BUTTON_DPAD_DOWN}, {"pad_esquerda", GLFW_GAMEPAD_BUTTON_DPAD_LEFT},
    {"pad_direita", GLFW_GAMEPAD_BUTTON_DPAD_RIGHT}};

// Entrada crua da janela: teclado (se a interface do editor não o estiver usando), mouse e o primeiro controle.
game::RawInput readRawInput(GLFWwindow* window, bool keyboard, bool mouse) {
  game::RawInput raw;
  if (keyboard)
    for (const auto& [name, k] : kGameKeys)
      if (glfwGetKey(window, k) == GLFW_PRESS) raw.held.insert(name);
  int w = 0, h = 0;
  glfwGetWindowSize(window, &w, &h);
  double mx = 0.0, my = 0.0;
  glfwGetCursorPos(window, &mx, &my);
  if (w > 0 && h > 0) raw.mouse = glm::vec2(float(mx / w), float(my / h));
  raw.mouseDown = mouse && glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
  for (int j = GLFW_JOYSTICK_1; j <= GLFW_JOYSTICK_LAST; ++j) {
    GLFWgamepadstate st;
    if (!glfwJoystickIsGamepad(j) || !glfwGetGamepadState(j, &st)) continue;
    raw.pad = true;
    for (const auto& [name, b] : kPadButtons)
      if (st.buttons[b] == GLFW_PRESS) raw.held.insert(name);
    const int axes[6] = {GLFW_GAMEPAD_AXIS_LEFT_X, GLFW_GAMEPAD_AXIS_LEFT_Y, GLFW_GAMEPAD_AXIS_RIGHT_X,
                         GLFW_GAMEPAD_AXIS_RIGHT_Y, GLFW_GAMEPAD_AXIS_LEFT_TRIGGER, GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER};
    for (int i = 0; i < 6; ++i) {
      float v = st.axes[axes[i]];
      if (i >= 4) v = 0.5f * (v + 1.0f);  // gatilhos: -1..1 -> 0..1
      else if (std::abs(v) < 0.15f) v = 0.0f;  // zona morta dos analógicos
      raw.axes[i] = v;
    }
    break;
  }
  return raw;
}

// Multijogador: --host abre o anfitrião, --conectar liga como cliente (cena .json com "multijogador").
std::unique_ptr<game::Network> startNetwork(int hostPort, const std::string& connectTo) {
  game::World* w = game::currentWorld();
  if (!w || (hostPort == 0 && connectTo.empty())) return nullptr;
  auto net = std::make_unique<game::Network>();
  if (hostPort > 0) {
    if (!net->host(*w, uint16_t(hostPort))) return nullptr;
  } else {
    std::string addr = connectTo;
    uint16_t port = 7777;
    if (const size_t c = addr.rfind(':'); c != std::string::npos) {
      port = uint16_t(std::atoi(addr.substr(c + 1).c_str()));
      addr = addr.substr(0, c);
    }
    if (!net->connect(*w, addr, port)) return nullptr;
  }
  return net;
}

// Câmera de cinema pedida por script (camera() / camera_jogo()).
// A câmera de antes da cena volta quando o script devolve o controle (camera_jogo): posição, direção e FOV.
void applyScriptCamera(const game::World& w, scene::Camera& camera) {
  static bool wasActive = false;
  static scene::Camera saved;
  if (!w.cameraOverride.active) {
    if (wasActive) {
      camera.position = saved.position;
      camera.yaw = saved.yaw;
      camera.pitch = saved.pitch;
      camera.fovY = saved.fovY;
      wasActive = false;
    }
    return;
  }
  if (!wasActive) {
    saved = camera;
    wasActive = true;
  }
  camera.position = w.cameraOverride.position;
  camera.lookAt(w.cameraOverride.target);
  if (w.cameraOverride.fovDeg > 0.0f) camera.fovY = glm::radians(w.cameraOverride.fovDeg);
}

// Personagem/carro do jogador: WASD ou analógico esquerdo andam; espaço ou A pula (no carro: freio de mão, também
// B); Shift, L3 ou LB correm; no carro, RT acelera e LT freia (somados ao W/S).
game::PlayerInput playerInputFrom(const game::GameInput& in, float cameraYaw) {
  game::PlayerInput p;
  glm::vec2 m(float(in.down("d")) - float(in.down("a")), float(in.down("w")) - float(in.down("s")));
  m += glm::vec2(in.axis("esquerdo_x"), -in.axis("esquerdo_y"));
  m.y += in.axis("rt") - in.axis("lt");
  if (glm::length(m) > 1.0f) m = glm::normalize(m);
  p.move = m;
  p.cameraYaw = cameraYaw;
  p.jump = in.down("espaco") || in.down("pad_a") || in.down("pad_b");
  p.run = in.down("shift") || in.down("pad_l3") || in.down("pad_lb");
  return p;
}

// Pedidos dos scripts que mexem no renderizador: opcao("motion_blur", v), opcao("dof", v), opcao("exposicao", ev).
void applyGameOptions(game::World& w, renderer::Settings& s) {
  for (const auto& [name, v] : w.optionRequests) {
    if (name == "motion_blur") s.motionBlur = v != 0.0f;
    else if (name == "dof") s.depthOfField = v != 0.0f;
    else if (name == "exposicao") s.exposureCompensation = v;
    else LOG_WARN("opcao: '%s' desconhecida (motion_blur, dof, exposicao)", name.c_str());
  }
  w.optionRequests.clear();
}


struct Options {
  bool headless = false;
  bool validation = false;
  bool rayTracing = true;
  bool vsync = true;
  uint32_t width = 1920, height = 1080;
  bool fullscreen = false;
  float scaleOverride = 0.0f;  // > 0: substitui a resolução interna do preset
  uint32_t frames = 32;  // frames renderizados no modo headless (o último é salvo)
  std::string out = "frame.png";
  uint32_t saveEvery = 0;  // > 0: salva também a cada N frames (frame_0007.png...)
  float move = 0.0f;       // headless: velocidade da câmera (m/s), para testar estabilidade temporal
  std::string cameraPath;  // headless: trajetória de câmera de cinema (arquivo de quadros-chave; ver loadCameraPath)
  float fps = 30.0f;       // headless: quadros por segundo do tempo simulado (vídeo)
  bool realTime = false;   // headless: cada quadro espera o relógio (rede e recarga de script em GPUs rápidas)
  uint32_t warmup = 0;     // headless: frames iniciais parados e não salvos (TAA, denoiser e caches se assentam)
  float quitAfter = 0.0f;  // janela: fecha sozinha depois de N segundos e imprime as estatísticas do tempo de quadro
  std::string saveWorld;   // cena .json: grava o estado do mundo (poses depois da física) ao terminar
  bool editor = false;     // janela: editor visual (painéis, seleção, alças)
  std::string screenshot;  // janela: grava a imagem da janela (com a interface) no último quadro
  std::string select;      // editor: entidade selecionada ao abrir (testes)
  std::string graph;  // --grafo: abre no editor de grafos (script visual ou material por nós)
  bool testRestore = false;  // sem janela: guarda as poses no início e volta a elas antes do --save-world (teste)
  std::string audioWav;      // sem janela: grava a mixagem do áudio 3D (ouvinte = câmera) em WAV
  std::string simulate;      // entradas simuladas (teclas, cliques, controle) para testes: ver game/input.h
  int hostPort = 0;          // --host [porta]: anfitrião de multijogador
  std::string connectTo;     // --conectar ip[:porta]: cliente
  uint32_t moveStop = 0;   // > 0: a câmera anda só nos primeiros N frames (referência na mesma pose final)
  bool pause = false;      // congela animações (medições de ruído com cena estática)
  bool streamSync = false;       // streaming: espera cada leitura do disco no mesmo frame (referência determinística)
  bool textureStreaming = true;  // mips finos das texturas sob demanda (--no-texture-streaming: tudo no início)
  uint32_t pauseAt = 0;    // > 0: as animações rodam N frames e congelam (referência no mesmo instante da cena)
  bool compressTextures = true;
  bool ignoreTangents = false;   // --ignore-tangents  // texturas em BC7 (--no-texture-compression = RGBA8)
  bool meshLights = true;        // triângulos emissivos como luzes (--no-mesh-lights: só pelos raios de GI)
  bool meshLods = true;          // níveis de detalhe das malhas grandes (--no-lod)
  int clusterLod = -1;           // geometria virtualizada: -1 = a da cena, 1 = --nanite, 0 = --no-nanite
  std::string tangentTest;      // --tangent-test arquivo.gltf: tangentes do arquivo x aproximação x MikkTSpace
  uint32_t atmosphereTest = 0;  // > 0: valida a atmosfera contra path tracing (caminhos por thread)
  float sunElevation = NAN, sunAzimuth = NAN;  // graus; NAN = o da cena
  scene::DemoOptions scene;
  bool customCamera = false;
  glm::vec3 cameraPos{0.0f}, cameraTarget{0.0f, 0.0f, -1.0f};
  renderer::Settings render;
};

Options parseArgs(int argc, char** argv) {
  bool noGiProbes = false;
  Options o;
  bool noFireflyClamp = false;
  int sppOverride = 0, candOverride = 0, neighOverride = -1;  // medição: substituem o preset (--spp, --restir-*)
  bool noBiasRays = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--headless") o.headless = true;
    else if (a == "--validation") o.validation = true;
    else if (a == "--no-rt") o.rayTracing = false;
    else if (a == "--no-vsync") o.vsync = false;
    else if (a == "--width") o.width = uint32_t(std::atoi(next()));
    else if (a == "--height") o.height = uint32_t(std::atoi(next()));
    else if (a == "--frames") o.frames = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--out") o.out = next();
    else if (a == "--save-every") o.saveEvery = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--move") o.move = float(std::atof(next()));
    else if (a == "--camera-path") o.cameraPath = next();
    else if (a == "--fps") o.fps = std::max(1.0f, float(std::atof(next())));
    else if (a == "--tempo-real") o.realTime = true;
    else if (a == "--warmup") o.warmup = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--quit-after") o.quitAfter = std::max(0.0f, float(std::atof(next())));
    else if (a == "--save-world") o.saveWorld = next();
    else if (a == "--editor") o.editor = true;
    else if (a == "--screenshot") o.screenshot = next();
    else if (a == "--select") o.select = next();
    else if (a == "--grafo") o.graph = next();
    else if (a == "--test-restore") o.testRestore = true;
    else if (a == "--audio-wav") o.audioWav = next();
    else if (a == "--simular") o.simulate = next();
    else if (a == "--idioma") game::setLanguageOverride(next());
    else if (a == "--host") {
      o.hostPort = 7777;
      if (i + 1 < argc && std::isdigit(static_cast<unsigned char>(argv[i + 1][0]))) o.hostPort = std::atoi(argv[++i]);
    }
    else if (a == "--conectar") o.connectTo = next();
    else if (a == "--move-stop") o.moveStop = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--pause") o.pause = true;
    else if (a == "--no-texture-compression") o.compressTextures = false;
    else if (a == "--no-mesh-lights") o.meshLights = false;
    else if (a == "--no-lod") o.meshLods = false;
    else if (a == "--no-nanite") o.clusterLod = 0;
    else if (a == "--nanite") o.clusterLod = 1;
    else if (a == "--no-lod-blocks") o.render.lodBlocks = false;
    else if (a == "--no-cluster-occlusion") o.render.clusterOcclusion = false;
    else if (a == "--cluster-occlusion-all") o.render.clusterOcclusionAll = true;
    else if (a == "--sw-raster") o.render.swRaster = true;
    else if (a == "--sw-raster-px") o.render.swRasterPixels = std::max(1.0f, float(std::atof(next())));
    else if (a == "--lod-pixels") o.render.lodPixels = std::max(0.01f, float(std::atof(next())));
    else if (a == "--no-firefly-clamp") noFireflyClamp = true;
    else if (a == "--pause-at") o.pauseAt = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--sun-elevation") o.sunElevation = float(std::atof(next()));
    else if (a == "--sun-azimuth") o.sunAzimuth = float(std::atof(next()));
    else if (a == "--ignore-tangents") o.ignoreTangents = true;
    else if (a == "--tangent-test") o.tangentTest = next();
    else if (a == "--atmosphere-test") o.atmosphereTest = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--scene") o.scene.name = next();
    else if (a == "--cam" || a == "--look") {
      glm::vec3 v(0.0f);
      if (std::sscanf(next(), "%f,%f,%f", &v.x, &v.y, &v.z) != 3) FATAL("%s espera x,y,z", a.c_str());
      (a == "--cam" ? o.cameraPos : o.cameraTarget) = v;
      o.customCamera = true;
    }
    else if (a == "--night") o.scene.night = true;
    else if (a == "--no-moon") o.scene.moon = false;
    else if (a == "--clouds") o.scene.clouds = float(std::atof(next()));
    else if (a == "--fog") o.scene.fog = float(std::atof(next()));
    else if (a == "--rain") o.scene.rain = float(std::atof(next()));
    else if (a == "--lights") o.scene.lightCount = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--city-blocks") o.scene.cityBlocks = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--rochas") o.scene.rocks = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--rocha-segmentos") o.scene.rockSegments = uint32_t(std::max(16, std::atoi(next())));
    else if (a == "--scale") o.scaleOverride = glm::clamp(float(std::atof(next())), 0.25f, 1.0f);
    else if (a == "--fullscreen") o.fullscreen = true;
    else if (a == "--quality") {
      renderer::QualityPreset p;
      const char* name = next();
      if (!renderer::parseQualityPreset(name, p)) FATAL("--quality: use baixo, medio, alto, ultra ou cinema (recebido '%s')", name);
      o.render.quality = renderer::qualityPreset(p);
    }
    else if (a == "--exposure") o.render.manualEV100 = float(std::atof(next()));
    else if (a == "--ev") o.render.exposureCompensation = float(std::atof(next()));
    else if (a == "--dof") o.render.depthOfField = true;
    else if (a == "--focus") o.render.dofFocusDistance = float(std::atof(next()));
    else if (a == "--no-denoiser") o.render.denoiser = false;
    else if (a == "--denoiser") {  // reblur | relax | svgf | dlss-rr
      std::string v = next();
      if (v == "dlss-rr") {  // DLSS Ray Reconstruction: denoiser + upscaler da NVIDIA (RTX)
        o.render.upscaler = renderer::Upscaler::Kind::DlssRR;
        v = "svgf";  // sem RTX, volta ao SVGF + TAAU do motor
      }
      else
        o.render.autoRayReconstruction = false;  // denoiser do motor pedido: o Auto não troca pelo DLSS-RR
      o.render.nrd = v != "svgf";
      o.render.nrdReblur = v == "reblur";
    }
    else if (a == "--nrd-validation") o.render.nrdValidation = true;
    else if (a == "--upscaler") {  // auto | taau | dlss | fsr
      std::string v = next();
      o.render.upscaler = v == "auto" ? renderer::Upscaler::Kind::Auto
                        : v == "dlss" ? renderer::Upscaler::Kind::Dlss
                        : v == "dlss-rr" ? renderer::Upscaler::Kind::DlssRR
                        : v == "fsr"  ? renderer::Upscaler::Kind::Fsr
                        : v == "nis"  ? renderer::Upscaler::Kind::Nis  // só teste da integração
                                      : renderer::Upscaler::Kind::None;
    }
    else if (a == "--sharpness") o.render.sharpness = glm::clamp(float(std::atof(next())), 0.0f, 1.0f);
    else if (a == "--no-motion-blur") o.render.motionBlur = false;
    else if (a == "--no-lens-flare") o.render.lensFlare = 0.0f;
    else if (a == "--shadow-maps") o.render.shadowMaps = true;
    else if (a == "--sondas-gi") o.render.giProbes = next();
    else if (a == "--assar-gi") o.render.giRebake = true;
    else if (a == "--sem-sondas-gi") noGiProbes = true;  // nem o arquivo automático (comparações)
    else if (a == "--gi-espacamento") o.render.giSpacing = float(std::atof(next()));
    else if (a == "--gi-raios") o.render.giRays = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--gi-rebotes") o.render.giBounces = uint32_t(std::max(1, std::atoi(next())));
    else if (a == "--no-gpu-culling") o.render.gpuCulling = false;
    else if (a == "--no-occlusion") o.render.occlusionCulling = false;
    else if (a == "--no-meshlets") o.render.meshlets = false;
    else if (a == "--no-hlod") o.render.hlod = false;
    else if (a == "--hlod") o.render.hlod = true;
    else if (a == "--no-ripples") o.render.ripples = false;
    else if (a == "--clouds-reference") o.render.cloudReference = true;
    else if (a == "--fog-reference") o.render.fogReference = true;
    else if (a == "--no-sss") o.render.subsurface = false;
    else if (a == "--all-shader-features") o.render.allShaderFeatures = true;
    else if (a == "--perf-skip") {  // medição: sol,gi,especular,sombras-acerto,secundarios,luz (imagem errada)
      std::string v = std::string(",") + next() + ",";
      const std::pair<const char*, uint32_t> kinds[] = {{"sol", 1u}, {"gi", 2u}, {"especular", 4u}, {"sombras-acerto", 8u},
                                                        {"secundarios", 16u | 64u}, {"luz", 32u},
                                                        {"espelho", 16u}, {"cache", 64u}, {"espelho-gi", 128u}, {"opaco", 256u}, {"gi-metade", 512u}, {"especular-fosco", 1024u}, {"cache-oitavo", 2048u},
                                                        {"sombra-acerto-opaca", 4096u},
                                                        {"diag-cache-direto", 16384u}, {"diag-cache-ref2", 32768u},
                                                        {"diag-taa-antigo", 65536u}};
      for (auto& [n, b] : kinds)
        if (v.find(std::string(",") + n + ",") != std::string::npos) o.render.perfSkipMask |= b;
    }
    else if (a == "--spp") sppOverride = int(std::atoi(next()));
    else if (a == "--restir-candidates") candOverride = int(std::atoi(next()));
    else if (a == "--restir-neighbors") neighOverride = int(std::atoi(next()));
    else if (a == "--no-restir-bias-rays") noBiasRays = true;
    else if (a == "--stream-sync") o.streamSync = true;
    else if (a == "--no-texture-streaming") o.textureStreaming = false;
    else if (a == "--texture-streaming") o.textureStreaming = true;
    else if (a == "--no-radiance-cache") o.render.radianceCache = false;
    else if (a == "--no-virtual-motion") o.render.virtualMotion = false;  // medição do SVGF
    else if (a == "--reference" || a == "--reference-pt") {  // verdade por acumulação: sem denoiser, TAA, upscaling nem blur
      o.render.referenceAccumulation = true;
      o.render.referencePathTrace = a == "--reference-pt";  // + rebotes por path tracing (verdade de GI e reflexos)
      o.render.denoiser = false;
      o.render.upscaler = renderer::Upscaler::Kind::None;
      o.render.motionBlur = false;
      o.render.sharpness = 0.0f;
      o.scaleOverride = 1.0f;
      o.textureStreaming = false;  // texturas inteiras desde o 1º frame acumulado (sem mips grossos no começo da média)
    }
    else if (a == "--no-energy-compensation") o.render.energyCompensation = false;
    else if (a == "--restir-no-reuse") {
      std::string v = next();
      o.render.restirDisableMask = v == "temporal" ? 1u : v == "spatial" ? 2u : 3u;
    } else if (a == "--debug-view") {
      const char* v = next();
      if (!renderer::parseDebugView(v, o.render.debugView)) {
        std::fprintf(stderr, "Visualização desconhecida: %s (use sol, sol-luz, normal, nl, luzes ou luzes-ref)\n", v);
        std::exit(1);
      }
    }
    else if (a == "--help" || a == "-h") {
      std::printf(
          "uso: legacy [opções]\n"
          "  --scene NOME|arquivo.gltf  cena: sponza (padrão), outdoor, city, ocean, river, world, stream, crowd, hair,\n"
          "                             cloth, scan, scan-backlit, skin, skin-backlit, glass, paint, reflections, vfx,\n"
          "                             shadows, shadows-far, mesh-lights*, furnace (teste da fornalha branca)\n"
          "  --night [--no-moon]      noite: luzes locais (ReSTIR), lua cheia e estrelas\n"
          "  --clouds X               cobertura de nuvens volumétricas 0..1 (0 = céu limpo)\n"
          "  --fog X                  névoa volumétrica: densidade no chão (1/m; 0,02 = neblina com raios de luz)\n"
          "  --rain X                 chuva 0..1: superfícies molhadas, poças com reflexo e respingos\n"
          "  --lights N               número de luzes pontuais\n"
          "  --scene city [--city-blocks N]  cidade procedural de NxN quarteirões (teste de escala; padrão 16)\n"
          "  --quality P              baixo | medio | alto | ultra (padrão) | cinema (sem economia)\n"
          "  --scale S                resolução interna (0.25-1.0; o preset define; menor = upscaling pelo TAA)\n"
          "  --fullscreen             tela cheia na resolução nativa do monitor\n"
          "  --dof [--focus M]        profundidade de campo (autofoco se --focus omitido)\n"
          "  --ev X | --exposure EV   compensação da auto-exposição (EV) | exposição manual (EV100)\n"
          "  --denoiser svgf|relax|reblur|dlss-rr  padrão: DLSS-RR na RTX, senão SVGF próprio; NVIDIA NRD ReLAX / ReBLUR [--nrd-validation]\n"
          "                             ou DLSS Ray Reconstruction (RTX: denoiser + upscaler numa rede só)\n"
          "  --no-denoiser --no-motion-blur --no-rt --no-vsync --validation\n"
          "  --cam x,y,z --look x,y,z  posição e alvo da câmera\n"
          "  --sun-elevation G --sun-azimuth G   posição do sol em graus\n"
          "  --debug-view sol|sol-luz|normal|nl|luzes|luzes-ref  valor cru (depuração)\n"
          "  --restir-no-reuse temporal|spatial|all   desliga reusos do ReSTIR (depuração)\n"
          "  --reference                referência por acumulação (sem denoiser; use muitos --frames com a câmera parada)\n"
          "  --reference-pt             idem, com os rebotes por path tracing (verdade de GI e reflexos)\n"
          "  --no-texture-compression   texturas em RGBA8 (padrão: BC7, 4x menos memória de vídeo)\n"
          "  --no-mesh-lights           emissivos sem amostragem de luz (só para medir as luzes de malha)\n"
          "  --no-firefly-clamp         sem limite de vagalumes (medição)\n"
          "  --no-radiance-cache        desliga o cache de radiância (rebotes além do 1º pelo céu aproximado)\n"
          "  --sharpness X              nitidez RCAS na saída (0 a 1; padrão 0,5, ou 0 com DLSS)\n"
          "  --upscaler auto|taau|dlss|fsr  auto (padrão) = DLSS em RTX, senão o TAA/TAAU do motor; ou AMD FSR 3; com --scale:\n"
          "                             1 = DLAA/nativo, 0,67 = qualidade, 0,58 = equilibrado, 0,5 = desempenho\n"
          "  --no-gpu-culling --no-occlusion --no-meshlets   desligam partes do culling/desenho (medição)\n"
          "  --nanite            geometria virtualizada (pedaços com erro de até --lod-pixels 1 na tela)\n"
          "  --hlod                   liga os proxies de HLOD (desligados: mais lentos na RTX)\n"
          "  --no-ripples --no-sss --no-texture-streaming --stream-sync  idem (ondulações, pele, texturas, streaming)\n"
          "  --clouds-reference         nuvens por Monte Carlo volumétrico exato (tools/nuvens.py)\n"
          "  --fog-reference            névoa integrada por pixel, sem grade (precisa de ray tracing; tools/nevoa.py)\n"
          "  --tangent-test arquivo.gltf  tangentes do arquivo contra a aproximação e o MikkTSpace\n"
          "  --atmosphere-test N       valida o céu contra path tracing (N caminhos por thread)\n"
          "  --width W --height H\n"
          "  --headless --frames N --out arquivo.png [--save-every N] [--move m/s [--move-stop N]] [--pause | --pause-at N]\n"
          "  --camera-path arquivo --fps N --warmup N   câmera de cinema por quadros-chave (vídeo: --save-every 1)\n"
          "  janela: --move m/s, --camera-path, --warmup N (quadros), --quit-after S (fecha e imprime o tempo de quadro)\n"
          "  --scene cena.json [--save-world saida.json]   cena em arquivo com física (ex.: cenas/fisica.json)\n"
          "  --audio-wav saida.wav    sem janela: grava o áudio 3D da cena .json (ouvinte = câmera)\n"
          "  --simular \"t:evento;...\"  entradas simuladas para testes (tecla, segurar, clique, mouse, eixo)\n"
          "  --idioma en         idioma dos textos do jogo (idiomas/<codigo>.json)\n"
          "  --tempo-real        sem janela: cada quadro espera o relógio (testes de rede e recarga)\n"
          "  --host [porta]           multijogador: anfitrião (padrão 7777; cena com \"multijogador\")\n"
          "  --conectar ip[:porta]    multijogador: entra no jogo de outro computador\n"
          "  --editor                 editor visual na janela (entidades, propriedades, alças, jogar/pausar, salvar)\n"
          "  --screenshot arq.png     janela: grava a tela (com a interface) ao fechar (use com --quit-after)\n");
      std::exit(0);
    }
  }
  if (o.scaleOverride > 0.0f) o.render.quality.renderScale = o.scaleOverride;
  // Medição: substituições do preset (valem depois do --quality, em qualquer ordem).
  if (sppOverride > 0) o.render.quality.samplesPerPixel = uint32_t(sppOverride);
  if (candOverride > 0) o.render.quality.restirCandidates = uint32_t(candOverride);
  if (neighOverride >= 0) o.render.quality.restirNeighbors = uint32_t(neighOverride);
  if (noBiasRays) o.render.quality.restirBiasCorrection = false;
  // Economias pedidas por --perf-skip (nomes da medição) viram as opções de qualidade correspondentes.
  {
    renderer::Quality& q = o.render.quality;
    uint32_t& m = o.render.perfSkipMask;
    if (m & 128u) q.mirrorGiCache = true;
    if (m & 512u) q.giHalf = true;
    if (m & 1024u) q.roughSpecCache = true;
    if (m & 2048u) q.cacheEighth = true;
    if (m & 4096u) q.hitShadowOpaque = true;
    m &= ~(128u | 512u | 1024u | 2048u | 4096u);
  }
  // Medição: sem o limite de "vagalumes" (o caminho ingênuo sem amostrar luzes perde energia nele).
  if (noFireflyClamp) o.render.quality.fireflyLimit = 1e30f;
  // Sondas de GI sem --sondas-gi: ao lado do arquivo da cena (cena.json.gi) ou, nas cenas de demonstração, em
  // texcache/ (uma por cena e por dia/noite). Usadas se existirem; --assar-gi grava nesse lugar.
  if (o.render.giProbes.empty() && !noGiProbes) {
    const std::string& n = o.scene.name;
    const bool file = n.find('.') != std::string::npos || n.find('/') != std::string::npos || n.find('\\') != std::string::npos;
    const std::string path = file ? n + ".gi" : "texcache/gi_" + n + (o.scene.night ? "_noite" : "") + ".gi";
    std::error_code ec;
    if (o.render.giRebake || std::filesystem::exists(path, ec)) o.render.giProbes = path;
    if (o.render.giRebake && !file) std::filesystem::create_directories("texcache", ec);
  }
  return o;
}

// Sobrescreve elevação/azimute do sol (graus) mantendo o que não foi informado.
void applySunOverride(const Options& opt, scene::Scene& scene) {
  if (std::isnan(opt.sunElevation) && std::isnan(opt.sunAzimuth)) return;
  glm::vec3 d = glm::normalize(scene.sunDirection);
  float elev = std::isnan(opt.sunElevation) ? std::asin(d.y) : glm::radians(opt.sunElevation);
  float azim = std::isnan(opt.sunAzimuth) ? std::atan2(d.x, -d.z) : glm::radians(opt.sunAzimuth);
  scene.sunDirection = {std::cos(elev) * std::sin(azim), std::sin(elev), -std::cos(elev) * std::cos(azim)};
  if (scene.sunIlluminance <= 0.0f && elev > -0.2f) scene.sunIlluminance = 120000.0f;
}

int runAtmosphereTest(const Options& opt) {
  rhi::Context ctx;
  ctx.createInstance({opt.validation, opt.rayTracing, {}});
  ctx.createDevice(VK_NULL_HANDLE);
  scene::Scene scene;
  scene::Camera camera;
  scene::buildDemoScene(scene, camera, {"furnace", false, 0});
  scene.compressTextures = opt.compressTextures;
  scene.meshLights = opt.meshLights;
  scene.meshLods = opt.meshLods;
  if (opt.clusterLod >= 0) scene.clusterLod = opt.clusterLod != 0;
  scene.streamSync = opt.streamSync;
  scene.textureStreaming = opt.textureStreaming;
  scene.upload(ctx);
  renderer::Renderer r;
  r.init(ctx, scene, {64, 64}, VK_FORMAT_R8G8B8A8_SRGB, opt.render);
  float err = r.runAtmosphereTest(opt.atmosphereTest);
  r.destroy();
  scene.destroy(ctx);
  ctx.destroy();
  return err < 0.1f ? 0 : 1;
}

// Câmera de cinema: arquivo de texto, uma linha por quadro-chave, "t px py pz alvox alvoy alvoz [fov]" (t em
// segundos, fov vertical em graus, padrão 60); linhas vazias e com # são ignoradas. Entre os quadros, Catmull-Rom
// na posição, no alvo e no fov (a curva passa pelos quadros-chave, sem quinas). Quadro-chave repetido no mesmo
// lugar = câmera parando suavemente ali.
struct CameraKey {
  float t;
  glm::vec3 pos, target;
  float fov;
};

std::vector<CameraKey> loadCameraPath(const std::string& path) {
  std::vector<CameraKey> keys;
  FILE* f = std::fopen(path.c_str(), "r");
  if (!f) FATAL("--camera-path: não abri %s", path.c_str());
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    const char* c = line;
    while (*c == ' ' || *c == '\t') ++c;
    if (*c == '#' || *c == '\n' || *c == '\r' || *c == 0) continue;
    CameraKey k{};
    k.fov = 60.0f;
    int n = std::sscanf(c, "%f %f %f %f %f %f %f %f", &k.t, &k.pos.x, &k.pos.y, &k.pos.z, &k.target.x, &k.target.y,
                        &k.target.z, &k.fov);
    if (n < 7) FATAL("--camera-path: linha inválida em %s: %s", path.c_str(), line);
    if (!keys.empty() && k.t <= keys.back().t) FATAL("--camera-path: os tempos precisam crescer (%s)", line);
    keys.push_back(k);
  }
  std::fclose(f);
  if (keys.empty()) FATAL("--camera-path: nenhum quadro-chave em %s", path.c_str());
  return keys;
}

template <typename T>
T catmullRom(const T& p0, const T& p1, const T& p2, const T& p3, float u) {
  const float u2 = u * u, u3 = u2 * u;
  return 0.5f * ((2.0f * p1) + (p2 - p0) * u + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * u2 +
                 (3.0f * p1 - p0 - 3.0f * p2 + p3) * u3);
}

void applyCameraPath(const std::vector<CameraKey>& keys, float t, scene::Camera& camera) {
  size_t i = 0;
  while (i + 1 < keys.size() && keys[i + 1].t <= t) ++i;
  if (i + 1 >= keys.size() || t <= keys[0].t) {  // antes do primeiro ou depois do último: parada
    const CameraKey& k = t <= keys[0].t ? keys[0] : keys.back();
    camera.position = k.pos;
    camera.lookAt(k.target);
    camera.fovY = glm::radians(k.fov);
    return;
  }
  const CameraKey& k0 = keys[i == 0 ? 0 : i - 1];
  const CameraKey& k1 = keys[i];
  const CameraKey& k2 = keys[i + 1];
  const CameraKey& k3 = keys[std::min(i + 2, keys.size() - 1)];
  const float u = (t - k1.t) / (k2.t - k1.t);
  camera.position = catmullRom(k0.pos, k1.pos, k2.pos, k3.pos, u);
  camera.lookAt(catmullRom(k0.target, k1.target, k2.target, k3.target, u));
  camera.fovY = glm::radians(catmullRom(k0.fov, k1.fov, k2.fov, k3.fov, u));
}

int runHeadless(const Options& opt) {
  rhi::Context ctx;
  ctx.createInstance({opt.validation, opt.rayTracing, {}});
  ctx.createDevice(VK_NULL_HANDLE);

  scene::Scene scene;
  scene::Camera camera;
  scene.ignoreFileTangents = opt.ignoreTangents;  // antes de carregar o glTF
  game::setAudioOffline(true);  // sem janela: sem placa de som (a mixagem vai para --audio-wav)
  if (!scene::buildDemoScene(scene, camera, opt.scene)) return 1;
  if (opt.testRestore && game::currentWorld()) game::currentWorld()->snapshot();  // o "Jogar" do editor
  if (opt.customCamera) {
    camera.position = opt.cameraPos;
    camera.lookAt(opt.cameraTarget);
  }
  applySunOverride(opt, scene);
  scene.compressTextures = opt.compressTextures;
  scene.meshLights = opt.meshLights;
  scene.meshLods = opt.meshLods;
  if (opt.clusterLod >= 0) scene.clusterLod = opt.clusterLod != 0;
  scene.streamSync = opt.streamSync;
  scene.textureStreaming = opt.textureStreaming;
  scene.upload(ctx);

  VkExtent2D extent{opt.width, opt.height};
  const VkFormat outFormat = VK_FORMAT_R8G8B8A8_SRGB;
  renderer::Renderer r;
  r.init(ctx, scene, extent, outFormat, opt.render);
  LOG_INFO("Resolução interna %ux%u -> saída %ux%u", r.renderExtent().width, r.renderExtent().height, extent.width,
           extent.height);

  rhi::Image output = rhi::createImage2D(ctx, extent, outFormat,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  rhi::Buffer readback = rhi::createBuffer(ctx, VkDeviceSize(extent.width) * extent.height * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu);

  auto savePng = [&](const std::string& path) {
    vmaInvalidateAllocation(ctx.allocator, readback.allocation, 0, VK_WHOLE_SIZE);
    if (util::writePng(path, static_cast<const uint8_t*>(readback.mapped), extent.width, extent.height)) {
      LOG_INFO("Imagem salva em %s", path.c_str());
      return true;
    }
    LOG_ERROR("Falha ao gravar %s", path.c_str());
    return false;
  };

  const float dt = 1.0f / opt.fps;  // tempo simulado fixo: resultado determinístico
  const std::vector<game::SimEvent> simEvents = game::parseSimulated(opt.simulate);
  std::unique_ptr<game::Network> net = startNetwork(opt.hostPort, opt.connectTo);
  glm::vec3 moveDir = camera.forward();
  const std::vector<CameraKey> cameraKeys = opt.cameraPath.empty() ? std::vector<CameraKey>{} : loadCameraPath(opt.cameraPath);
  if (!cameraKeys.empty()) {
    LOG_INFO("Câmera de cinema: %zu quadros-chave, %.2f s, %.0f quadros/s, %u de aquecimento", cameraKeys.size(),
             cameraKeys.back().t, opt.fps, opt.warmup);
    applyCameraPath(cameraKeys, 0.0f, camera);
  }
  auto t0 = std::chrono::steady_clock::now();
  double cpuRecordMs = 0.0;  // CPU gravando o frame (atualização da cena, BVH de luzes, chamadas de desenho), sem a GPU
  bool ok = true;
  // Medição: com 20 frames ou mais, os tempos (GPU, CPU e o total) contam só a segunda metade; a primeira inclui a
  // placa subindo o clock depois do carregamento (na RTX, a mesma configuração variava até 30% entre execuções).
  const uint32_t timedFrom = opt.frames >= 20 ? opt.frames / 2 : 0;
  const auto realStart = std::chrono::steady_clock::now();
  for (uint32_t f = 0; f < opt.frames; ++f) {
    // --tempo-real: o quadro f começa em f·dt segundos de relógio (numa GPU rápida, sem isso 900 quadros passam em
    // 1 s e um cliente de rede nem chega a conectar).
    if (opt.realTime)
      std::this_thread::sleep_until(realStart + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                    std::chrono::duration<double>(double(f) * dt)));
    if (f == timedFrom && timedFrom > 0) {
      (void)r.gpuTimings();
      (void)r.cpuTimings();
      t0 = std::chrono::steady_clock::now();
      cpuRecordMs = 0.0;
    }
    bool last = f + 1 == opt.frames;
    const bool warming = f < opt.warmup;  // aquecimento: câmera e cena paradas no início, nada salvo
    const uint32_t shot = warming ? 0 : f - opt.warmup;  // índice do quadro do vídeo
    bool save = last || (!warming && opt.saveEvery && (shot + 1) % opt.saveEvery == 0);
    if (!cameraKeys.empty()) applyCameraPath(cameraKeys, float(shot) * dt, camera);
    if (opt.move != 0.0f && (opt.moveStop == 0 || f < opt.moveStop)) camera.position += moveDir * (opt.move * dt);
    const bool frozen = warming || opt.pause || (opt.pauseAt != 0 && f >= opt.pauseAt);
    if (game::World* w = game::currentWorld()) {
      w->setListener(camera.position, camera.forward());
      // Sem janela não há teclado nem controle: só as entradas simuladas (--simular), no tempo do quadro.
      game::updateInput(w->gameInput, game::RawInput{}, simEvents, (float(f) - 1.0f) * dt, float(f) * dt);
      w->setPlayerInput(playerInputFrom(w->gameInput, camera.yaw));
      if (net) w->networkStep = [&net, &scene, &camera, w](float stepDt) {
        net->update(*w, scene, stepDt, playerInputFrom(w->gameInput, camera.yaw));
      };
      applyGameOptions(*w, r.settings);
      w->qualityRequest.clear();  // sem janela a qualidade fica a da linha de comando
    }
    scene.update(frozen ? 0.0f : dt);
    if (game::World* w = game::currentWorld()) applyScriptCamera(*w, camera);
    scene.stream(ctx, camera.position);  // streaming do mundo (células perto da câmera)
    if (!frozen) r.restartReference();  // --reference: só acumula com a cena parada
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
      auto c0 = std::chrono::steady_clock::now();
      r.record(cmd, f % renderer::Renderer::kFramesInFlight, camera, output.handle, output.view, dt);
      if (f > timedFrom || (timedFrom == 0 && f > 0)) cpuRecordMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
      if (!save) return;
      rhi::imageBarrier(cmd, output.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {extent.width, extent.height, 1};
      vkCmdCopyImageToBuffer(cmd, output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
    });
    if (f == 0 || last || (f + 1) % 16 == 0) LOG_INFO("frame %u/%u", f + 1, opt.frames);
    if (save && (!last || opt.saveEvery)) {  // em vídeo (--save-every) o último quadro também entra na sequência
      char name[32];
      std::snprintf(name, sizeof(name), "_%04u.png", shot + 1);
      std::string path = opt.out;
      size_t dot = path.rfind(".png");
      path = (dot == std::string::npos ? path : path.substr(0, dot)) + name;
      ok &= savePng(path);
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  const uint32_t timed = opt.frames - timedFrom;
  LOG_INFO("%u frames em %.2fs (%.1f ms/frame, inclui espera da CPU)%s", timed, secs, secs * 1000.0 / timed,
           timedFrom ? " — segunda metade" : "");
  if (timed > 1) LOG_INFO("CPU gravando o frame: %.2f ms/frame", cpuRecordMs / double(timed - 1));
  if (!scene.textures.empty()) LOG_INFO("Texturas na GPU no fim: %.0f MB", double(scene.textureBytes()) / 1048576.0);
  if (std::string t = r.gpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
  if (std::string t = r.cpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
  ok &= savePng(opt.out);
  if (!opt.audioWav.empty()) {
    if (game::World* w = game::currentWorld(); w && w->audio) ok &= w->audio->writeWav(opt.audioWav);
    else LOG_WARN("--audio-wav: a cena não é um arquivo .json com mundo");
  }
  if (!opt.saveWorld.empty()) {
    if (game::World* w = game::currentWorld()) {
      if (opt.testRestore) w->restore(scene);  // o "Parar" do editor
      ok &= w->save(opt.saveWorld);
    }
    else LOG_WARN("--save-world: a cena não é um arquivo .json com mundo");
  }

  rhi::destroyBuffer(ctx, readback);
  rhi::destroyImage(ctx, output);
  if (const char* dump = std::getenv("LEGACY_HAIR_DUMP")) r.dumpHair(dump);
  if (const char* dump = std::getenv("LEGACY_CLOTH_DUMP")) r.dumpCloth(dump);
  r.destroy();
  scene.destroy(ctx);
  ctx.destroy();
  return ok ? 0 : 1;
}

struct FrameSync {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence inFlight = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
};

int runWindowed(const Options& opt) {
  if (!glfwInit()) FATAL("Falha ao iniciar GLFW");
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWmonitor* monitor = nullptr;
  int winW = int(opt.width), winH = int(opt.height);
  if (opt.fullscreen) {
    monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* mode = glfwGetVideoMode(monitor);
    winW = mode->width;
    winH = mode->height;
    glfwWindowHint(GLFW_REFRESH_RATE, mode->refreshRate);
  }
  GLFWwindow* window = glfwCreateWindow(winW, winH, "Legacy Engine", monitor, nullptr);
  if (!window) FATAL("Falha ao criar janela");

  uint32_t extCount = 0;
  const char** glfwExts = glfwGetRequiredInstanceExtensions(&extCount);
  rhi::Context ctx;
  ctx.createInstance({opt.validation, opt.rayTracing, std::vector<const char*>(glfwExts, glfwExts + extCount)});
  VkSurfaceKHR surface;
  VK_CHECK(glfwCreateWindowSurface(ctx.instance, window, nullptr, &surface));
  ctx.createDevice(surface);

  scene::Scene scene;
  scene::Camera camera;
  // Monta a cena (no início e quando o editor pede para recarregar depois de criar ou apagar entidades).
  auto buildScene = [&](const std::string& name) {
    scene::DemoOptions so = opt.scene;
    so.name = name;
    scene.ignoreFileTangents = opt.ignoreTangents;  // antes de carregar o glTF
    if (!scene::buildDemoScene(scene, camera, so)) return false;
    applySunOverride(opt, scene);
    scene.compressTextures = opt.compressTextures;
    scene.meshLights = opt.meshLights;
    scene.meshLods = opt.meshLods;
  if (opt.clusterLod >= 0) scene.clusterLod = opt.clusterLod != 0;
    scene.streamSync = opt.streamSync;
    scene.textureStreaming = opt.textureStreaming;
    scene.upload(ctx);
    return true;
  };
  if (!buildScene(opt.scene.name)) return 1;
  if (opt.customCamera) {
    camera.position = opt.cameraPos;
    camera.lookAt(opt.cameraTarget);
  }

  int fbw, fbh;
  glfwGetFramebufferSize(window, &fbw, &fbh);
  rhi::Swapchain swapchain;
  swapchain.create(ctx, surface, {uint32_t(fbw), uint32_t(fbh)}, opt.vsync);
  renderer::Renderer r;
  r.init(ctx, scene, swapchain.extent, swapchain.format, opt.render);

  std::array<FrameSync, renderer::Renderer::kFramesInFlight> frames;
  for (auto& f : frames) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.queueFamily;
    VK_CHECK(vkCreateCommandPool(ctx.device, &pci, nullptr, &f.pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = f.pool;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(ctx.device, &ai, &f.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VK_CHECK(vkCreateFence(ctx.device, &fci, nullptr, &f.inFlight));
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx.device, &sci, nullptr, &f.imageAvailable));
  }

  bool resized = false;
  glfwSetWindowUserPointer(window, &resized);
  glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) { *static_cast<bool*>(glfwGetWindowUserPointer(w)) = true; });

  editor::Editor ed;
  // Cena de jogo (.json) sem o editor: só a interface de jogo (HUD dos scripts).
  if (opt.editor || game::currentWorld())
    ed.init(ctx, window, swapchain.format, uint32_t(swapchain.images.size()), opt.editor);
  if (ed.ready() && !opt.select.empty() && game::currentWorld()) ed.selectByName(*game::currentWorld(), opt.select);
  if (ed.ready() && !opt.graph.empty()) ed.openGraph(opt.graph);
  rhi::Buffer shotBuffer{};  // --screenshot: cópia da imagem da janela no último quadro
  VkExtent2D shotExtent{};

  auto recreate = [&]() {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    while (w == 0 || h == 0) { glfwWaitEvents(); glfwGetFramebufferSize(window, &w, &h); }
    vkDeviceWaitIdle(ctx.device);
    swapchain.create(ctx, surface, {uint32_t(w), uint32_t(h)}, opt.vsync);
    r.resize(swapchain.extent);
  };

  LOG_INFO("Controles: botão direito + mouse = olhar | WASD = mover | Q/E = descer/subir | Shift = rápido");
  LOG_INFO("           setas = mover o sol | +/- = exposição (EV) | F = foco/DOF | M = motion blur | Esc = sair");

  uint32_t slot = 0;
  double lastTime = glfwGetTime(), fpsTimer = lastTime, lastX = 0, lastY = 0;
  uint32_t fpsFrames = 0;
  // Medição na janela (--quit-after): câmera automática (--camera-path ou --move), quadros de aquecimento parados
  // (--warmup) e, ao fechar, média e percentis do tempo de quadro real (do present ao present, com CPU e GPU em
  // paralelo como num jogo) da segunda metade do tempo medido.
  const std::vector<CameraKey> cameraKeys = opt.cameraPath.empty() ? std::vector<CameraKey>{} : loadCameraPath(opt.cameraPath);
  const glm::vec3 moveDir = camera.forward();
  uint32_t frameCount = 0;
  double runStart = -1.0;  // início do trecho medido (depois do aquecimento)
  std::vector<double> frameMs;
  if (!cameraKeys.empty()) applyCameraPath(cameraKeys, 0.0f, camera);
  bool looking = false;
  const double startTime = glfwGetTime();
  float tPrevInput = -1.0f;
  const std::vector<game::SimEvent> simEvents = game::parseSimulated(opt.simulate);
  std::unique_ptr<game::Network> net = startNetwork(opt.hostPort, opt.connectTo);
  float sunElev = std::asin(scene.sunDirection.y);
  float sunAzim = std::atan2(scene.sunDirection.x, -scene.sunDirection.z);

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    double now = glfwGetTime();
    float dt = float(now - lastTime);
    lastTime = now;
    // Esc fecha o programa só fora de um jogo com menu (no jogo, Esc abre o menu de pausa).
    {
      game::World* gw = game::currentWorld();
      const bool hasMenu = gw && !gw->registry.view<game::SystemEntity>().empty();
      if (!hasMenu && glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);
    }
    if (ed.ready()) ed.frame(game::currentWorld(), scene, camera, swapchain.extent, dt * 1000.0f);
    ++frameCount;
    const bool warming = frameCount <= opt.warmup;
    if (!warming && runStart < 0.0) runStart = now;
    const double runTime = runStart < 0.0 ? 0.0 : now - runStart;
    if (!warming && frameCount > opt.warmup + 1) frameMs.push_back(double(dt) * 1000.0);
    if (opt.quitAfter > 0.0f && runTime >= opt.quitAfter) glfwSetWindowShouldClose(window, 1);
    if (!cameraKeys.empty()) applyCameraPath(cameraKeys, float(runTime), camera);
    else if (opt.move != 0.0f && !warming) camera.position += moveDir * (opt.move * dt);

    // ---- Entrada ----
    bool moved = false;
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS && !ed.capturingMouse();
    if (rmb && looking && (mx != lastX || my != lastY)) {
      camera.yaw += float(mx - lastX) * 0.0025f;
      camera.pitch = glm::clamp(camera.pitch - float(my - lastY) * 0.0025f, -1.55f, 1.55f);
      moved = true;
    }
    looking = rmb;
    lastX = mx;
    lastY = my;
    float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 40.0f : 6.0f) * dt;
    auto key = [&](int k) { return !ed.capturingKeyboard() && glfwGetKey(window, k) == GLFW_PRESS; };
    glm::vec3 move(0.0f);
    if (key(GLFW_KEY_W)) move += camera.forward();
    if (key(GLFW_KEY_S)) move -= camera.forward();
    if (key(GLFW_KEY_D)) move += camera.right();
    if (key(GLFW_KEY_A)) move -= camera.right();
    if (key(GLFW_KEY_E)) move.y += 1.0f;
    if (key(GLFW_KEY_Q)) move.y -= 1.0f;
    // Cena com jogador (personagem com "controle": "jogador"): WASD anda, espaço pula, Shift corre; a câmera segue
    // atrás dele (botão direito + mouse gira). Sem jogador: câmera livre.
    game::World* world = game::currentWorld();
    if (world) {
      const float tNow = float(now - startTime);
      game::updateInput(world->gameInput, readRawInput(window, !ed.capturingKeyboard(), !ed.capturingMouse()), simEvents,
                        tPrevInput, tNow);
      tPrevInput = tNow;
      // Analógico direito gira a câmera (como o mouse com o botão direito).
      camera.yaw += world->gameInput.axis("direito_x") * 2.5f * dt;
      camera.pitch = glm::clamp(camera.pitch - world->gameInput.axis("direito_y") * 1.8f * dt, -1.55f, 1.55f);
    }
    glm::vec3 playerFeet(0.0f);
    // No editor parado (editando), a câmera fica livre mesmo com jogador na cena.
    glm::vec3 carPos(0.0f), carFwd(0.0f, 0.0f, 1.0f);
    const bool hasCar = world && world->simulate && world->playerVehicle(carPos, carFwd);
    const bool hasPlayer = world && world->simulate && (hasCar || world->playerFeet(playerFeet));
    if (hasPlayer) {
      world->setPlayerInput(playerInputFrom(world->gameInput, camera.yaw));
      if (net) world->networkStep = [&net, &scene, &camera, w = world](float stepDt) {
        net->update(*w, scene, stepDt, playerInputFrom(w->gameInput, camera.yaw));
      };
    } else if (glm::length(move) > 0.0f) { camera.position += glm::normalize(move) * speed; moved = true; }
    float sunStep = world ? 0.0f : 0.6f * dt;  // controles de desenvolvimento (sol, exposição): só fora de um jogo
    if (key(GLFW_KEY_UP)) { sunElev += sunStep; moved = true; }
    if (key(GLFW_KEY_DOWN)) { sunElev -= sunStep; moved = true; }
    if (key(GLFW_KEY_LEFT)) { sunAzim -= sunStep; moved = true; }
    if (key(GLFW_KEY_RIGHT)) { sunAzim += sunStep; moved = true; }
    sunElev = glm::clamp(sunElev, -0.1f, 1.55f);
    scene.sunDirection = {std::cos(sunElev) * std::sin(sunAzim), std::sin(sunElev), -std::cos(sunElev) * std::cos(sunAzim)};
    if (!world && (key(GLFW_KEY_EQUAL) || key(GLFW_KEY_KP_ADD))) r.settings.exposureCompensation += dt;
    if (!world && (key(GLFW_KEY_MINUS) || key(GLFW_KEY_KP_SUBTRACT))) r.settings.exposureCompensation -= dt;
    auto toggle = [&](int k, bool& state, bool& setting) {
      bool down = key(k);
      if (down && !state) setting = !setting;
      state = down;
    };
    static bool fDown = false, mDown = false;
    if (!world) {
      toggle(GLFW_KEY_F, fDown, r.settings.depthOfField);
      toggle(GLFW_KEY_M, mDown, r.settings.motionBlur);
    }
    (void)moved;  // o histórico temporal é reprojetado: mover a câmera não reinicia nada

    if (world) world->setListener(camera.position, camera.forward());
    scene.update(opt.pause ? 0.0f : glm::clamp(dt, 0.0f, 0.1f));
    if (world) {
      if (world->quitRequested) glfwSetWindowShouldClose(window, 1);
      applyGameOptions(*world, r.settings);
    }
    if (hasCar && world->playerVehicle(carPos, carFwd)) {  // câmera de perseguição: 7 m atrás, 2,5 m acima
      const glm::vec3 flat = glm::length(glm::vec3(carFwd.x, 0.0f, carFwd.z)) > 1e-3f
                                 ? glm::normalize(glm::vec3(carFwd.x, 0.0f, carFwd.z)) : glm::vec3(0.0f, 0.0f, 1.0f);
      camera.position = carPos - flat * 7.0f + glm::vec3(0.0f, 2.5f, 0.0f);
      camera.lookAt(carPos + glm::vec3(0.0f, 1.0f, 0.0f));
    } else if (hasPlayer && world->playerFeet(playerFeet)) {  // terceira pessoa: 4,5 m atrás da cabeça
      const glm::vec3 head = playerFeet + glm::vec3(0.0f, 1.6f, 0.0f);
      camera.position = head - camera.forward() * 4.5f + glm::vec3(0.0f, 0.4f, 0.0f);
    }
    if (world) applyScriptCamera(*world, camera);
    scene.stream(ctx, camera.position);

    // ---- Frame ----
    FrameSync& f = frames[slot];
    VK_CHECK(vkWaitForFences(ctx.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX));
    uint32_t imageIndex;
    VkResult acq = vkAcquireNextImageKHR(ctx.device, swapchain.handle, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreate(); continue; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) FATAL("vkAcquireNextImageKHR: %d", acq);
    VK_CHECK(vkResetFences(ctx.device, 1, &f.inFlight));

    VK_CHECK(vkResetCommandBuffer(f.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    r.record(f.cmd, slot, camera, swapchain.images[imageIndex], swapchain.views[imageIndex], glm::clamp(dt, 0.0f, 0.1f));
    ed.render(f.cmd, swapchain.views[imageIndex], swapchain.extent);
    if (!opt.screenshot.empty() && glfwWindowShouldClose(window) && shotBuffer.handle == VK_NULL_HANDLE) {
      shotExtent = swapchain.extent;
      shotBuffer = rhi::createBuffer(ctx, VkDeviceSize(shotExtent.width) * shotExtent.height * 4,
                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu);
      rhi::imageBarrier(f.cmd, swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {shotExtent.width, shotExtent.height, 1};
      vkCmdCopyImageToBuffer(f.cmd, swapchain.images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shotBuffer.handle, 1, &region);
      rhi::imageBarrier(f.cmd, swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    }
    rhi::imageBarrier(f.cmd, swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_NONE, 0);
    VK_CHECK(vkEndCommandBuffer(f.cmd));

    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f.imageAvailable;
    wait.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = swapchain.renderFinished[imageIndex];
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    VK_CHECK(vkQueueSubmit2(ctx.queue, 1, &si, f.inFlight));

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &swapchain.renderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain.handle;
    pi.pImageIndices = &imageIndex;
    VkResult pres = vkQueuePresentKHR(ctx.queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR || resized) {
      resized = false;
      recreate();
    } else if (pres != VK_SUCCESS) {
      FATAL("vkQueuePresentKHR: %d", pres);
    }
    slot = (slot + 1) % renderer::Renderer::kFramesInFlight;

    // Opções do jogo: outra qualidade => o renderizador é refeito no lugar com o preset (a cena na GPU continua).
    if (game::World* w = game::currentWorld(); w && !w->qualityRequest.empty()) {
      renderer::QualityPreset preset;
      if (renderer::parseQualityPreset(w->qualityRequest.c_str(), preset)) {
        vkDeviceWaitIdle(ctx.device);
        renderer::Settings keep = r.settings;
        keep.quality = renderer::qualityPreset(preset);
        r.destroy();
        r.~Renderer();
        new (&r) renderer::Renderer();
        r.init(ctx, scene, swapchain.extent, swapchain.format, keep);
        LOG_INFO("Qualidade trocada para %s", w->qualityRequest.c_str());
        lastTime = glfwGetTime();
      } else {
        LOG_WARN("qualidade: '%s' desconhecida (baixo, medio, alto, ultra, cinema)", w->qualityRequest.c_str());
      }
      w->qualityRequest.clear();
    }

    // Editor: entidade criada ou apagada => a cena foi gravada num arquivo temporário e é montada de novo (a GPU
    // recebe tudo outra vez). A câmera continua onde estava.
    if (std::string reload; ed.consumeReload(reload)) {
      vkDeviceWaitIdle(ctx.device);
      const scene::Camera keep = camera;
      r.destroy();
      scene.destroy(ctx);
      // Objetos refeitos no lugar (mesmo tipo, não constantes): os nomes `scene` e `r` passam a se referir aos novos.
      scene.~Scene();
      new (&scene) scene::Scene();
      r.~Renderer();
      new (&r) renderer::Renderer();
      if (!buildScene(reload)) FATAL("Editor: não consegui recarregar %s", reload.c_str());
      camera = keep;
      if (game::World* w = game::currentWorld()) w->simulate = false;  // continua no modo de edição
      r.init(ctx, scene, swapchain.extent, swapchain.format, opt.render);
      lastTime = glfwGetTime();
    }

    ++fpsFrames;
    if (now - fpsTimer >= 0.5) {
      char title[160];
      std::snprintf(title, sizeof(title), "Legacy Engine | %.0f FPS (%.2f ms) | %ux%u -> %ux%u | RT: %s",
                    fpsFrames / (now - fpsTimer), 1000.0 * (now - fpsTimer) / fpsFrames, r.renderExtent().width,
                    r.renderExtent().height, swapchain.extent.width, swapchain.extent.height, ctx.rayTracing ? "on" : "off");
      glfwSetWindowTitle(window, title);
      fpsTimer = now;
      fpsFrames = 0;
      static double lastReport = 0.0;
      if (now - lastReport >= 3.0) {  // detalhamento por passe no console a cada 3 s
        if (std::string t = r.gpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
        if (std::string t = r.cpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
        lastReport = now;
      }
    }
  }

  vkDeviceWaitIdle(ctx.device);
  if (shotBuffer.handle != VK_NULL_HANDLE) {
    vmaInvalidateAllocation(ctx.allocator, shotBuffer.allocation, 0, VK_WHOLE_SIZE);
    std::vector<uint8_t> px(static_cast<const uint8_t*>(shotBuffer.mapped),
                            static_cast<const uint8_t*>(shotBuffer.mapped) + size_t(shotExtent.width) * shotExtent.height * 4);
    if (swapchain.format == VK_FORMAT_B8G8R8A8_SRGB || swapchain.format == VK_FORMAT_B8G8R8A8_UNORM)
      for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // BGRA -> RGBA
    for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
    if (util::writePng(opt.screenshot, px.data(), shotExtent.width, shotExtent.height)) LOG_INFO("Tela salva em %s", opt.screenshot.c_str());
    rhi::destroyBuffer(ctx, shotBuffer);
  }
  ed.shutdown();
  if (opt.quitAfter > 0.0f && frameMs.size() >= 4) {
    std::vector<double> v(frameMs.begin() + std::ptrdiff_t(frameMs.size() / 2), frameMs.end());  // segunda metade
    double sum = 0.0;
    for (double x : v) sum += x;
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, size_t(p * double(v.size() - 1) + 0.5))]; };
    LOG_INFO("Janela %ux%u (interna %ux%u): %zu quadros medidos, média %.2f ms (%.1f quadros/s), mediana %.2f ms, "
             "p95 %.2f ms, p99 %.2f ms, pior %.2f ms", swapchain.extent.width, swapchain.extent.height,
             r.renderExtent().width, r.renderExtent().height, v.size(), sum / double(v.size()),
             1000.0 * double(v.size()) / sum, pct(0.5), pct(0.95), pct(0.99), v.back());
    if (std::string t = r.gpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
  }
  for (auto& f : frames) {
    vkDestroySemaphore(ctx.device, f.imageAvailable, nullptr);
    vkDestroyFence(ctx.device, f.inFlight, nullptr);
    vkDestroyCommandPool(ctx.device, f.pool, nullptr);
  }
  r.destroy();
  swapchain.destroy(ctx);
  scene.destroy(ctx);
  vkDestroySurfaceKHR(ctx.instance, surface, nullptr);
  ctx.destroy();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}


}  // namespace

#ifdef _WIN32
// Console do Windows em UTF-8 (acentos das mensagens), sem incluir <windows.h>.
extern "C" __declspec(dllimport) int __stdcall SetConsoleOutputCP(unsigned int codePage);
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(65001);
#endif
  Options opt = parseArgs(argc, argv);
  if (!opt.tangentTest.empty()) return scene::tangentTest(opt.tangentTest);
  if (opt.atmosphereTest) return runAtmosphereTest(opt);
  return opt.headless ? runHeadless(opt) : runWindowed(opt);
}
