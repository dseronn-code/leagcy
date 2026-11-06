// Pênalti da Treta para Android: o Legacy Engine numa NativeActivity (Vulkan direto na janela do Android).
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
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/paths.h"
#include "game/penalti_game.h"
#include "renderer/renderer.h"
#include "rhi/swapchain.h"
#include "scene/demo_scene.h"

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
  game::PenaltiGame game;
  std::array<FrameSync, renderer::Renderer::kFramesInFlight> frames{};
  uint32_t slot = 0;
  ANativeWindow* window = nullptr;
  bool rendererReady = false, swapchainReady = false, sceneReady = false;

  void init(ANativeWindow* w, const std::string& cacheDir) {
    window = w;
    ctx.createInstance({false, true, {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME}});
    VkAndroidSurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    si.window = window;
    VK_CHECK(vkCreateAndroidSurfaceKHR(ctx.instance, &si, nullptr, &surface));
    ctx.createDevice(surface);
    LOG_INFO("GPU: %s | ray tracing: %s", ctx.properties.deviceName, ctx.rayTracing ? "sim" : "não");

    scene::DemoOptions opt;
    opt.name = "penalti";
    if (!scene::buildDemoScene(scene, camera, opt)) FATAL("Não foi possível montar o estádio (arquivos do jogo faltando).");
    scene.textureCacheDir = cacheDir + "/texcache";
    scene.textureStreaming = false;
    scene.upload(ctx);
    sceneReady = true;

    swapchain.create(ctx, surface, {uint32_t(ANativeWindow_getWidth(window)), uint32_t(ANativeWindow_getHeight(window))}, true);
    swapchainReady = true;
    // Preset de celular: resolução interna de 50% reconstruída pelo TAAU, 1 amostra, ReSTIR enxuto, sem motion blur
    // pesado; o ray tracing liga sozinho nos celulares que têm (Adreno 740+, Immortalis, Xclipse).
    renderer::Settings rs;
    rs.quality = renderer::qualityPreset(renderer::QualityPreset::Low);
    rs.quality.renderScale = 0.5f;
    rs.motionBlur = true;
    rs.lensFlare = 0.6f;
    r.init(ctx, scene, swapchain.extent, swapchain.format, rs);
    rendererReady = true;
    game.init(scene, r, false, ctx.rayTracing);

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

  void frame(float dt, const game::Pointer& p) {
    const glm::vec2 screen(float(swapchain.extent.width), float(swapchain.extent.height));
    const float sceneDt = game.update(dt, p, scene, camera, r, screen);
    scene.update(sceneDt);
    FrameSync& f = frames[slot];
    VK_CHECK(vkWaitForFences(ctx.device, 1, &f.inFlight, VK_TRUE, UINT64_MAX));
    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(ctx.device, swapchain.handle, UINT64_MAX, f.imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { recreateSwapchain(); return; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) FATAL("vkAcquireNextImageKHR: %d", acq);
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
    VK_CHECK(vkQueueSubmit2(ctx.queue, 1, &si, f.inFlight));
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
  bool focused = false, failed = false;
  game::Pointer pointer;
  bool down = false, wasDown = false;
};

void startEngine(App& a) {
  if (a.engine || a.failed || !a.app->window) return;
  try {
    a.engine = std::make_unique<Engine>();
    a.engine->init(a.app->window, a.cacheDir);
  } catch (const std::exception& e) {
    a.failed = true;
    if (a.engine) { try { a.engine->destroy(); } catch (...) {} a.engine.reset(); }
    showError(a.app, std::string("O Pênalti da Treta não conseguiu iniciar neste celular:\n\n") + e.what());
  }
}

void handleCmd(android_app* app, int32_t cmd) {
  App& a = *static_cast<App*>(app->userData);
  switch (cmd) {
    case APP_CMD_INIT_WINDOW: startEngine(a); break;
    case APP_CMD_TERM_WINDOW:
      if (a.engine) { a.engine->destroy(); a.engine.reset(); }
      break;
    case APP_CMD_GAINED_FOCUS: a.focused = true; break;
    case APP_CMD_LOST_FOCUS: a.focused = false; break;
    default: break;
  }
}

int32_t handleInput(android_app* app, AInputEvent* event) {
  App& a = *static_cast<App*>(app->userData);
  if (AInputEvent_getType(event) != AINPUT_EVENT_TYPE_MOTION) return 0;
  const int32_t action = AMotionEvent_getAction(event) & AMOTION_EVENT_ACTION_MASK;
  a.pointer.pos = glm::vec2(AMotionEvent_getX(event, 0), AMotionEvent_getY(event, 0));
  if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN) a.down = true;
  else if (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_CANCEL) a.down = false;
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
    const bool animating = a.engine && a.focused;
    while (ALooper_pollOnce(animating ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
      if (source) source->process(app, source);
      if (app->destroyRequested) break;
    }
    if (app->destroyRequested) break;
    if (!a.engine || !a.focused) { last = std::chrono::steady_clock::now(); continue; }
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1f);
    last = now;
    game::Pointer p = a.pointer;
    p.down = a.down;
    p.pressed = a.down && !a.wasDown;
    p.released = !a.down && a.wasDown;
    a.wasDown = a.down;
    try {
      a.engine->frame(dt, p);
    } catch (const std::exception& e) {
      a.failed = true;
      a.engine->destroy();
      a.engine.reset();
      showError(app, std::string("Erro durante o jogo:\n\n") + e.what());
    }
  }
  if (a.engine) a.engine->destroy();
}
