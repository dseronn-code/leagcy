// Legacy Engine — ponto de entrada.
//   legacy                      janela interativa (WASD + botão direito do mouse)
//   legacy --headless --frames 64 --out frame.png
#include <chrono>
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
  uint32_t width = 1600, height = 900;
  uint32_t frames = 64;  // amostras acumuladas no modo headless
  std::string out = "frame.png";
  float exposure = -1.0f;
};

Options parseArgs(int argc, char** argv) {
  Options o;
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
    else if (a == "--exposure") o.exposure = float(std::atof(next()));
    else if (a == "--help" || a == "-h") {
      std::printf("uso: legacy [--headless] [--frames N] [--out arquivo.png] [--width W] [--height H]\n"
                  "              [--no-rt] [--no-vsync] [--validation] [--exposure X]\n");
      std::exit(0);
    }
  }
  return o;
}

int runHeadless(const Options& opt) {
  rhi::Context ctx;
  ctx.createInstance({opt.validation, opt.rayTracing, {}});
  ctx.createDevice(VK_NULL_HANDLE);

  scene::Scene scene;
  scene::Camera camera;
  scene::buildDemoScene(scene, camera);
  scene.upload(ctx);

  VkExtent2D extent{opt.width, opt.height};
  const VkFormat outFormat = VK_FORMAT_R8G8B8A8_SRGB;
  renderer::Renderer r;
  r.init(ctx, scene, extent, outFormat);
  if (opt.exposure > 0) r.settings.exposure = opt.exposure;

  rhi::Image output = rhi::createImage2D(ctx, extent, outFormat,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  rhi::Buffer readback = rhi::createBuffer(ctx, VkDeviceSize(extent.width) * extent.height * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuToCpu);

  auto t0 = std::chrono::steady_clock::now();
  for (uint32_t f = 0; f < opt.frames; ++f) {
    bool last = f + 1 == opt.frames;
    ctx.immediateSubmit([&](VkCommandBuffer cmd) {
      r.record(cmd, f % renderer::Renderer::kFramesInFlight, camera, output.handle, output.view);
      if (!last) return;
      rhi::imageBarrier(cmd, output.handle, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {extent.width, extent.height, 1};
      vkCmdCopyImageToBuffer(cmd, output.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.handle, 1, &region);
    });
    if (f == 0 || last || (f + 1) % 16 == 0) LOG_INFO("amostra %u/%u", f + 1, opt.frames);
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  LOG_INFO("%u frames em %.2fs (%.1f ms/frame)", opt.frames, secs, secs * 1000.0 / opt.frames);

  vmaInvalidateAllocation(ctx.allocator, readback.allocation, 0, VK_WHOLE_SIZE);
  bool ok = util::writePng(opt.out, static_cast<const uint8_t*>(readback.mapped), extent.width, extent.height);
  if (ok) LOG_INFO("Imagem salva em %s", opt.out.c_str());
  else LOG_ERROR("Falha ao gravar %s", opt.out.c_str());

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
  GLFWwindow* window = glfwCreateWindow(int(opt.width), int(opt.height), "Legacy Engine", nullptr, nullptr);
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
  scene::buildDemoScene(scene, camera);
  scene.upload(ctx);

  int fbw, fbh;
  glfwGetFramebufferSize(window, &fbw, &fbh);
  rhi::Swapchain swapchain;
  swapchain.create(ctx, surface, {uint32_t(fbw), uint32_t(fbh)}, opt.vsync);
  renderer::Renderer r;
  r.init(ctx, scene, swapchain.extent, swapchain.format);
  if (opt.exposure > 0) r.settings.exposure = opt.exposure;

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
  LOG_INFO("           setas = mover o sol | +/- = exposição | Esc = sair");

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
    if (key(GLFW_KEY_EQUAL) || key(GLFW_KEY_KP_ADD)) r.settings.exposure *= 1.0f + dt;
    if (key(GLFW_KEY_MINUS) || key(GLFW_KEY_KP_SUBTRACT)) r.settings.exposure /= 1.0f + dt;
    if (moved) r.resetAccumulation();

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
    r.record(f.cmd, slot, camera, swapchain.images[imageIndex], swapchain.views[imageIndex]);
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
      std::snprintf(title, sizeof(title), "Legacy Engine | %.0f FPS (%.2f ms) | %u amostras | RT: %s",
                    fpsFrames / (now - fpsTimer), 1000.0 * (now - fpsTimer) / fpsFrames, r.accumulatedFrames(),
                    ctx.rayTracing ? "on" : "off");
      glfwSetWindowTitle(window, title);
      fpsTimer = now;
      fpsFrames = 0;
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

int main(int argc, char** argv) {
  Options opt = parseArgs(argc, argv);
  return opt.headless ? runHeadless(opt) : runWindowed(opt);
}
