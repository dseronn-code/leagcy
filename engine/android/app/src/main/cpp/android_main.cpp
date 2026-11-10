// FCLegacy para Android: o Legacy Engine numa NativeActivity (Vulkan direto na janela do Android).
// Ciclo: ao ganhar a janela, monta contexto, cena, swapchain e renderer; ao perder, desmonta tudo. Os shaders e os
// arquivos do jogo vêm dentro do APK e são copiados para a pasta interna do app na primeira abertura (o motor lê
// arquivos comuns). Toque = Pointer do jogo. Erro fatal (celular sem um recurso que o motor exige) vira uma caixa
// de diálogo com a mensagem, já que no celular não há console.
#include <android/asset_manager.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <sys/stat.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "game/partida_game.h"
#include "renderer/renderer.h"
#include "rhi/swapchain.h"
#include "scene/demo_scene.h"

void legacyVk13SetInstance(VkInstance instance);  // vk13_shim.cpp

// Log: vai para o logcat e as últimas linhas ficam na memória, para o vigia mostrar onde o jogo parou.
namespace {
std::mutex gLogMutex;
std::deque<std::string> gLogLines;
std::string recentLog(size_t n) {
  std::lock_guard<std::mutex> lock(gLogMutex);
  std::string out;
  for (size_t i = gLogLines.size() > n ? gLogLines.size() - n : 0; i < gLogLines.size(); ++i) out += gLogLines[i] + "\n";
  return out;
}
}  // namespace
void legacyLog(int priority, const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  __android_log_write(priority, "Legacy", buf);
  std::lock_guard<std::mutex> lock(gLogMutex);
  std::string line = buf;
  if (line.size() > 160) line = line.substr(0, 160) + "...";
  gLogLines.push_back(line);
  if (gLogLines.size() > 40) gLogLines.pop_front();
}

namespace {

// Copia uma pasta de dentro do APK para `dest` (só os arquivos que faltam ou mudaram de tamanho).
void extractAssetDir(AAssetManager* am, const std::string& dir, const std::string& dest) {
  mkdir(dest.c_str(), 0755);
  AAssetDir* d = AAssetManager_openDir(am, dir.c_str());
  if (!d) return;
  while (const char* name = AAssetDir_getNextFileName(d)) {
    const std::string src = dir + "/" + name, out = dest + "/" + name;
    AAsset* a = AAssetManager_open(am, src.c_str(), AASSET_MODE_STREAMING);
    if (!a) continue;
    const off_t size = AAsset_getLength(a);
    struct stat st{};
    if (stat(out.c_str(), &st) == 0 && st.st_size == size) { AAsset_close(a); continue; }
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    char buf[65536];
    int n;
    while ((n = AAsset_read(a, buf, sizeof(buf))) > 0) f.write(buf, n);
    AAsset_close(a);
  }
  AAssetDir_close(d);
}

// Mostra a mensagem numa caixa de diálogo (PenaltiActivity.showError) e espera o app ser fechado.
void showError(android_app* app, const std::string& message) {
  JNIEnv* env = nullptr;
  app->activity->vm->AttachCurrentThread(&env, nullptr);
  jclass cls = env->GetObjectClass(app->activity->clazz);
  jmethodID m = env->GetMethodID(cls, "showError", "(Ljava/lang/String;)V");
  if (m) {
    jstring s = env->NewStringUTF(message.c_str());
    env->CallVoidMethod(app->activity->clazz, m, s);
    env->DeleteLocalRef(s);
  }
  app->activity->vm->DetachCurrentThread();
}

struct FrameSync {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence inFlight = VK_NULL_HANDLE;
  VkSemaphore imageAvailable = VK_NULL_HANDLE;
};

// Tudo o que depende da janela: criado em INIT_WINDOW, destruído em TERM_WINDOW.
struct Engine {
  rhi::Context ctx;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  scene::Scene scene;
  scene::Camera camera;
  rhi::Swapchain swapchain;
  renderer::Renderer r;
  game::PartidaGame game;
  std::array<FrameSync, renderer::Renderer::kFramesInFlight> frames{};
  uint32_t slot = 0, frameCount = 0;
  std::atomic<uint32_t> framesPresented{0};
  ANativeWindow* window = nullptr;
  bool rendererReady = false, swapchainReady = false, sceneReady = false;

  void init(ANativeWindow* w, const std::string& cacheDir) {
    window = w;
    // Ray tracing desligado no celular por enquanto: o compilador de shaders do Adreno recusa os shaders grandes de
    // ray query (ReSTIR); o caminho sem RT (sombras em cascata + SSAO) é o testado para o preset de celular.
    ctx.createInstance({false, false, {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME}});
    legacyVk13SetInstance(ctx.instance);
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(ctx.instance, &si, nullptr, &surface));
    ctx.createDevice(surface);
    LOG_INFO("GPU: %s | ray tracing: %s", ctx.properties.deviceName, ctx.rayTracing ? "sim" : "não");

    scene::DemoOptions opt;
    opt.name = "partida";
    LOG_INFO("Etapa: montando o estádio");
    if (!scene::buildDemoScene(scene, camera, opt)) FATAL("Não foi possível montar o estádio (arquivos do jogo faltando).");
    scene.textureCacheDir = cacheDir + "/texcache";
    scene.textureStreaming = false;
    LOG_INFO("Etapa: enviando a cena para a GPU");
    scene.upload(ctx);
    sceneReady = true;
    LOG_INFO("Etapa: criando a tela (swapchain)");

    swapchain.create(ctx, surface, {uint32_t(ANativeWindow_getWidth(window)), uint32_t(ANativeWindow_getHeight(window))}, true);
    swapchainReady = true;
    // Preset de celular: resolução interna de 50% reconstruída pelo TAAU, 1 amostra, ReSTIR enxuto, sem motion blur
    // pesado; sem ray tracing por enquanto.
    renderer::Settings rs;
    rs.quality = renderer::qualityPreset(renderer::QualityPreset::Low);
    rs.quality.renderScale = 0.5f;
    rs.motionBlur = true;
    rs.lensFlare = 0.6f;
    LOG_INFO("Etapa: criando o renderer (%ux%u)", swapchain.extent.width, swapchain.extent.height);
    r.init(ctx, scene, swapchain.extent, swapchain.format, rs);
    rendererReady = true;
    LOG_INFO("Etapa: iniciando a partida");
    game.init(scene, r, false, ctx.rayTracing);
    ctx.checkPipelineErrors();

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
  }

  void recreateSwapchain() {
    vkDeviceWaitIdle(ctx.device);
    swapchain.create(ctx, surface, {uint32_t(ANativeWindow_getWidth(window)), uint32_t(ANativeWindow_getHeight(window))}, true);
    r.resize(swapchain.extent);
  }

  void frame(float dt, const std::vector<game::Touch>& touches) {
    const glm::vec2 screen(float(swapchain.extent.width), float(swapchain.extent.height));
    const bool trace = frameCount < 3;  // os primeiros frames deixam rastro no log (diagnóstico de tela preta)
    if (trace) LOG_INFO("Frame %u: jogo", frameCount);
    const float sceneDt = game.update(dt, touches, scene, camera, r, screen);
    if (trace) LOG_INFO("Frame %u: cena", frameCount);
    scene.update(sceneDt);
    if (trace) LOG_INFO("Frame %u: esperando a GPU", frameCount);
    FrameSync& f = frames[slot];
    const uint64_t kTimeout = 8'000'000'000ull;  // 8 s: a GPU travada vira erro com o log, não tela preta
    const VkResult wf = vkWaitForFences(ctx.device, 1, &f.inFlight, VK_TRUE, kTimeout);
    if (wf == VK_TIMEOUT)
      FATAL("A GPU não terminou o frame %u em 8 s (travou); %s", frameCount, r.gpuProgress(slot).c_str());
    if (wf != VK_SUCCESS) FATAL("vkWaitForFences: %d (frame %u)", int(wf), frameCount);
    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(ctx.device, swapchain.handle, kTimeout, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (acq == VK_TIMEOUT) FATAL("A tela não liberou imagem em 8 s (frame %u)", frameCount);
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) FATAL("vkAcquireNextImageKHR: %d", acq);
    if (trace) LOG_INFO("Frame %u: gravando", frameCount);
    VK_CHECK(vkResetFences(ctx.device, 1, &f.inFlight));
    VK_CHECK(vkResetCommandBuffer(f.cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f.cmd, &bi));
    r.record(f.cmd, slot, camera, swapchain.images[imageIndex], swapchain.views[imageIndex], dt);
    rhi::imageBarrier(f.cmd, swapchain.images[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
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
    if (trace) LOG_INFO("Frame %u: enviando", frameCount);
    VK_CHECK(vkQueueSubmit2(ctx.queue, 1, &si, f.inFlight));
    if (trace) LOG_INFO("Frame %u: apresentando", frameCount);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &swapchain.renderFinished[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain.handle;
    pi.pImageIndices = &imageIndex;
    const VkResult pres = vkQueuePresentKHR(ctx.queue, &pi);
    if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) recreateSwapchain();
    else if (pres != VK_SUCCESS) FATAL("vkQueuePresentKHR: %d", pres);
    slot = (slot + 1) % renderer::Renderer::kFramesInFlight;
    if (trace) LOG_INFO("Frame %u: apresentado", frameCount);
    ++frameCount;
    framesPresented.store(frameCount);
  }

  void destroy() {
    if (ctx.device) vkDeviceWaitIdle(ctx.device);
    for (auto& f : frames) {
      if (f.imageAvailable) vkDestroySemaphore(ctx.device, f.imageAvailable, nullptr);
      if (f.inFlight) vkDestroyFence(ctx.device, f.inFlight, nullptr);
      if (f.pool) vkDestroyCommandPool(ctx.device, f.pool, nullptr);
      f = FrameSync{};
    }
    if (rendererReady) r.destroy();
    if (swapchainReady) swapchain.destroy(ctx);
    if (sceneReady) scene.destroy(ctx);
    if (surface) vkDestroySurfaceKHR(ctx.instance, surface, nullptr);
    if (ctx.instance) ctx.destroy();
  }
};

struct App {
  android_app* app = nullptr;
  std::unique_ptr<Engine> engine;
  std::string cacheDir;
  // Desenha sempre que a janela existe e o app não está pausado. O foco não conta: painéis por cima do jogo (o Game
  // Booster da Samsung, notificações) tiram o foco e, antes, deixavam a tela preta.
  bool paused = false;
  std::atomic<bool> failed{false};
  std::vector<game::Touch> touches;  // dedos na tela agora (joystick + botões ao mesmo tempo)
};

// Vigia: se em 40 s nenhum frame chegou à tela, mostra as últimas linhas do log (onde o jogo parou).
void startWatchdog(App& a) {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([&a] {
    for (int s = 0; s < 40; ++s) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      if (a.failed || a.app->destroyRequested) return;
      Engine* e = a.engine.get();
      if (e && e->framesPresented.load() > 0) return;
    }
    if (a.failed) return;
    a.failed = true;
    showError(a.app, "O FCLegacy travou antes de mostrar a primeira imagem (40 s). Últimas etapas:\n\n" + recentLog(14));
  }).detach();
}

void startEngine(App& a) {
  if (a.engine || a.failed || !a.app->window) return;
  startWatchdog(a);
  try {
    a.engine = std::make_unique<Engine>();
    a.engine->init(a.app->window, a.cacheDir);
  } catch (const std::exception& e) {
    a.failed = true;
    showError(a.app, std::string("O FCLegacy não conseguiu iniciar neste celular:\n\n") + e.what() + "\n\nÚltimas linhas:\n" +
                         recentLog(6));
    a.engine.release();  // sem desmontar (GPU travada não volta do vkDeviceWaitIdle); o diálogo fecha o app
    return;
  }
}

void handleCmd(android_app* app, int32_t cmd) {
  App& a = *static_cast<App*>(app->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW: startEngine(a); break;
    case APP_CMD_TERM_WINDOW:
      if (a.engine) { a.engine->destroy(); a.engine.reset(); }
      break;
    case APP_CMD_RESUME: a.paused = false; break;
    case APP_CMD_PAUSE: a.paused = true; break;
    default: break;
  }
}

int32_t handleInput(android_app* app, AInputEvent* event) {
  App& a = *static_cast<App*>(app->userData);
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
  const int32_t full = AMotionEvent_getAction(event);
  const int32_t action = full & AMOTION_EVENT_ACTION_MASK;
  const size_t changed = size_t((full & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
  a.touches.clear();
  if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) return 1;
  const size_t n = AMotionEvent_getPointerCount(event);
  for (size_t i = 0; i < n; ++i) {
    if (action == AMOTION_EVENT_ACTION_POINTER_UP && i == changed) continue;  // esse dedo saiu
    a.touches.push_back({int(AMotionEvent_getPointerId(event, i)), glm::vec2(AMotionEvent_getX(event, i), AMotionEvent_getY(event, i))});
  }
  return 1;
}

}  // namespace

void android_main(android_app* app) {
  App a;
  a.app = app;
  app->userData = &a;
  app->onAppCmd = handleCmd;
  app->onInputEvent = handleInput;

  // Arquivos do APK -> pasta interna; o motor e o cache de texturas usam caminhos comuns.
  const std::string internal = app->activity->internalDataPath ? app->activity->internalDataPath : "/data/local/tmp";
  a.cacheDir = internal;
  extractAssetDir(app->activity->assetManager, "shaders", internal + "/shaders");
  extractAssetDir(app->activity->assetManager, "game", internal + "/game");
  core::setDataDirs(internal + "/shaders", internal + "/game");
  setenv("TMPDIR", internal.c_str(), 1);  // arquivos temporários do streaming

  auto last = std::chrono::steady_clock::now();
  while (!app->destroyRequested) {
    int events = 0;
    android_poll_source* source = nullptr;
    // O prazo é recalculado a cada evento: o motor nasce DENTRO de um evento (INIT_WINDOW). Calculado uma vez só,
    // o laço seguia esperando eventos para sempre depois da carga, e o primeiro frame nunca era desenhado.
    while (ALooper_pollOnce((a.engine && !a.paused) ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source) source->process(app, source);
      if (app->destroyRequested) break;
    }
    if (app->destroyRequested) break;
    if (!a.engine || a.paused) { last = std::chrono::steady_clock::now(); continue; }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;
    try {
      a.engine->frame(dt, a.touches);
    } catch (const std::exception& e) {
      // Mostra antes de desmontar: com a GPU travada, desmontar (vkDeviceWaitIdle) pode nunca voltar.
      const bool already = a.failed.exchange(true);
      if (!already) showError(app, std::string("Erro durante o jogo:\n\n") + e.what() + "\n\nÚltimas linhas:\n" + recentLog(8));
      a.engine.release();  // não desmonta: o app vai fechar pelo diálogo
    }
  }
  if (a.engine) a.engine->destroy();
}
