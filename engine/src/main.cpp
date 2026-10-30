// Legacy Engine — ponto de entrada.
//   legacy                      janela interativa (WASD + botão direito do mouse)
//   legacy --headless --frames 32 --out frame.png
//   legacy --scene outdoor --night --lights 1000
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "renderer/renderer.h"
#include "rhi/swapchain.h"
#include "scene/demo_scene.h"
#include "util/png.h"

namespace {

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
  uint32_t moveStop = 0;   // > 0: a câmera anda só nos primeiros N frames (referência na mesma pose final)
  bool pause = false;      // congela animações (medições de ruído com cena estática)
  uint32_t pauseAt = 0;    // > 0: as animações rodam N frames e congelam (referência no mesmo instante da cena)
  bool compressTextures = true;  // texturas em BC7 (--no-texture-compression = RGBA8)
  bool meshLights = true;        // triângulos emissivos como luzes (--no-mesh-lights: só pelos raios de GI)
  bool meshLods = true;          // níveis de detalhe das malhas grandes (--no-lod)
  uint32_t atmosphereTest = 0;  // > 0: valida a atmosfera contra path tracing (caminhos por thread)
  float sunElevation = NAN, sunAzimuth = NAN;  // graus; NAN = o da cena
  scene::DemoOptions scene;
  bool customCamera = false;
  glm::vec3 cameraPos{0.0f}, cameraTarget{0.0f, 0.0f, -1.0f};
  renderer::Settings render;
};

Options parseArgs(int argc, char** argv) {
  Options o;
  bool noFireflyClamp = false;
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
    else if (a == "--move-stop") o.moveStop = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--pause") o.pause = true;
    else if (a == "--no-texture-compression") o.compressTextures = false;
    else if (a == "--no-mesh-lights") o.meshLights = false;
    else if (a == "--no-lod") o.meshLods = false;
    else if (a == "--no-firefly-clamp") noFireflyClamp = true;
    else if (a == "--pause-at") o.pauseAt = uint32_t(std::max(0, std::atoi(next())));
    else if (a == "--sun-elevation") o.sunElevation = float(std::atof(next()));
    else if (a == "--sun-azimuth") o.sunAzimuth = float(std::atof(next()));
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
    else if (a == "--no-gpu-culling") o.render.gpuCulling = false;
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
          "  --scene sponza|outdoor|furnace|arquivo.gltf   cena (furnace = teste da fornalha branca)\n"
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
          "  --atmosphere-test N       valida o céu contra path tracing (N caminhos por thread)\n"
          "  --width W --height H\n"
          "  --headless --frames N --out arquivo.png [--save-every N] [--move m/s [--move-stop N]] [--pause | --pause-at N]\n");
      std::exit(0);
    }
  }
  if (o.scaleOverride > 0.0f) o.render.quality.renderScale = o.scaleOverride;
  // Medição: sem o limite de "vagalumes" (o caminho ingênuo sem amostrar luzes perde energia nele).
  if (noFireflyClamp) o.render.quality.fireflyLimit = 1e30f;
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
  scene.upload(ctx);
  renderer::Renderer r;
  r.init(ctx, scene, {64, 64}, VK_FORMAT_R8G8B8A8_SRGB, opt.render);
  float err = r.runAtmosphereTest(opt.atmosphereTest);
  r.destroy();
  scene.destroy(ctx);
  ctx.destroy();
  return err < 0.1f ? 0 : 1;
}

int runHeadless(const Options& opt) {
  rhi::Context ctx;
  ctx.createInstance({opt.validation, opt.rayTracing, {}});
  ctx.createDevice(VK_NULL_HANDLE);

  scene::Scene scene;
  scene::Camera camera;
  if (!scene::buildDemoScene(scene, camera, opt.scene)) return 1;
  if (opt.customCamera) {
    camera.position = opt.cameraPos;
    camera.lookAt(opt.cameraTarget);
  }
  applySunOverride(opt, scene);
  scene.compressTextures = opt.compressTextures;
  scene.meshLights = opt.meshLights;
  scene.meshLods = opt.meshLods;
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

  const float dt = 1.0f / 30.0f;  // tempo simulado fixo: resultado determinístico
  glm::vec3 moveDir = camera.forward();
  auto t0 = std::chrono::steady_clock::now();
  double cpuRecordMs = 0.0;  // CPU gravando o frame (atualização da cena, BVH de luzes, chamadas de desenho), sem a GPU
  bool ok = true;
  for (uint32_t f = 0; f < opt.frames; ++f) {
    bool last = f + 1 == opt.frames;
    bool save = last || (opt.saveEvery && (f + 1) % opt.saveEvery == 0);
    if (opt.move != 0.0f && (opt.moveStop == 0 || f < opt.moveStop)) camera.position += moveDir * (opt.move * dt);
    const bool frozen = opt.pause || (opt.pauseAt != 0 && f >= opt.pauseAt);
    scene.update(frozen ? 0.0f : dt);
    if (!frozen) r.restartReference();  // --reference: só acumula com a cena parada
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
      auto c0 = std::chrono::steady_clock::now();
      r.record(cmd, f % renderer::Renderer::kFramesInFlight, camera, output.handle, output.view, dt);
      if (f > 0) cpuRecordMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
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
    if (save && !last) {
      char name[32];
      std::snprintf(name, sizeof(name), "_%04u.png", f + 1);
      std::string path = opt.out;
      size_t dot = path.rfind(".png");
      path = (dot == std::string::npos ? path : path.substr(0, dot)) + name;
      ok &= savePng(path);
    }
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  LOG_INFO("%u frames em %.2fs (%.1f ms/frame, inclui espera da CPU)", opt.frames, secs, secs * 1000.0 / opt.frames);
  if (opt.frames > 1) LOG_INFO("CPU gravando o frame: %.2f ms/frame", cpuRecordMs / double(opt.frames - 1));
  if (std::string t = r.gpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
  if (std::string t = r.cpuTimings(); !t.empty()) LOG_INFO("%s", t.c_str());
  ok &= savePng(opt.out);

  rhi::destroyBuffer(ctx, readback);
  rhi::destroyImage(ctx, output);
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
  if (!scene::buildDemoScene(scene, camera, opt.scene)) return 1;
  if (opt.customCamera) {
    camera.position = opt.cameraPos;
    camera.lookAt(opt.cameraTarget);
  }
  applySunOverride(opt, scene);
  scene.compressTextures = opt.compressTextures;
  scene.meshLights = opt.meshLights;
  scene.meshLods = opt.meshLods;
  scene.upload(ctx);

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
  bool looking = false;
  float sunElev = std::asin(scene.sunDirection.y);
  float sunAzim = std::atan2(scene.sunDirection.x, -scene.sunDirection.z);

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();
    double now = glfwGetTime();
    float dt = float(now - lastTime);
    lastTime = now;
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, 1);

    // ---- Entrada ----
    bool moved = false;
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    if (rmb && looking && (mx != lastX || my != lastY)) {
      camera.yaw += float(mx - lastX) * 0.0025f;
      camera.pitch = glm::clamp(camera.pitch - float(my - lastY) * 0.0025f, -1.55f, 1.55f);
      moved = true;
    }
    looking = rmb;
    lastX = mx;
    lastY = my;
    float speed = (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 40.0f : 6.0f) * dt;
    auto key = [&](int k) { return glfwGetKey(window, k) == GLFW_PRESS; };
    glm::vec3 move(0.0f);
    if (key(GLFW_KEY_W)) move += camera.forward();
    if (key(GLFW_KEY_S)) move -= camera.forward();
    if (key(GLFW_KEY_D)) move += camera.right();
    if (key(GLFW_KEY_A)) move -= camera.right();
    if (key(GLFW_KEY_E)) move.y += 1.0f;
    if (key(GLFW_KEY_Q)) move.y -= 1.0f;
    if (glm::length(move) > 0.0f) { camera.position += glm::normalize(move) * speed; moved = true; }
    float sunStep = 0.6f * dt;
    if (key(GLFW_KEY_UP)) { sunElev += sunStep; moved = true; }
    if (key(GLFW_KEY_DOWN)) { sunElev -= sunStep; moved = true; }
    if (key(GLFW_KEY_LEFT)) { sunAzim -= sunStep; moved = true; }
    if (key(GLFW_KEY_RIGHT)) { sunAzim += sunStep; moved = true; }
    sunElev = glm::clamp(sunElev, -0.1f, 1.55f);
    scene.sunDirection = {std::cos(sunElev) * std::sin(sunAzim), std::sin(sunElev), -std::cos(sunElev) * std::cos(sunAzim)};
    if (key(GLFW_KEY_EQUAL) || key(GLFW_KEY_KP_ADD)) r.settings.exposureCompensation += dt;
    if (key(GLFW_KEY_MINUS) || key(GLFW_KEY_KP_SUBTRACT)) r.settings.exposureCompensation -= dt;
    auto toggle = [&](int k, bool& state, bool& setting) {
      bool down = key(k);
      if (down && !state) setting = !setting;
      state = down;
    };
    static bool fDown = false, mDown = false;
    toggle(GLFW_KEY_F, fDown, r.settings.depthOfField);
    toggle(GLFW_KEY_M, mDown, r.settings.motionBlur);
    (void)moved;  // o histórico temporal é reprojetado: mover a câmera não reinicia nada

    scene.update(opt.pause ? 0.0f : glm::clamp(dt, 0.0f, 0.1f));

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
  if (opt.atmosphereTest) return runAtmosphereTest(opt);
  return opt.headless ? runHeadless(opt) : runWindowed(opt);
}
