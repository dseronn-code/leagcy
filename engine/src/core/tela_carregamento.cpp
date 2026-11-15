#include "core/tela_carregamento.h"

#include <algorithm>
#include <vector>

#include <GLFW/glfw3.h>
#include <stb_image.h>

#include "core/log.h"
#include "rhi/context.h"
#include "rhi/swapchain.h"

namespace {
bool formatoSrgb(VkFormat f) {
  return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

void barreira(VkCommandBuffer cmd, VkImage img, VkImageLayout de, VkImageLayout para, VkAccessFlags srcA, VkAccessFlags dstA,
              VkPipelineStageFlags srcS, VkPipelineStageFlags dstS, uint32_t mips = 1) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = de;
  b.newLayout = para;
  b.srcAccessMask = srcA;
  b.dstAccessMask = dstA;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
  vkCmdPipelineBarrier(cmd, srcS, dstS, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Cópia (com escala) de um retângulo da origem para um retângulo da tela.
void copiar(VkCommandBuffer cmd, const rhi::Image& src, int sx0, int sx1, VkImage dst, int x0, int y0, int x1, int y1) {
  if (x1 <= x0 || y1 <= y0 || sx1 <= sx0) return;
  VkImageBlit r{};
  r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.srcOffsets[0] = {sx0, 0, 0};
  r.srcOffsets[1] = {sx1, int32_t(src.extent.height), 1};
  r.dstOffsets[0] = {x0, y0, 0};
  r.dstOffsets[1] = {x1, y1, 1};
  vkCmdBlitImage(cmd, src.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                 VK_FILTER_LINEAR);
}
}  // namespace

void TelaCarregamento::iniciar(rhi::Context& ctx, rhi::Swapchain& sc, VkSurfaceKHR surface, GLFWwindow* janela,
                               const std::string& imagem, bool vsync) {
  ctx_ = &ctx;
  sc_ = &sc;
  surface_ = surface;
  janela_ = janela;
  vsync_ = vsync;
  srgb_ = formatoSrgb(sc.format);
  VkFormatProperties fp{};
  vkGetPhysicalDeviceFormatProperties(ctx.gpu, sc.format, &fp);
  blit_ = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vkCreateFence(ctx.device, &fci, nullptr, &fence_);
  if (!blit_) {
    LOG_WARN("Tela de carregamento: a janela não aceita cópias de imagem; só o fundo");
    return;
  }
  // Imagens: a do produto (do disco), o trilho da barra (cinza) e a barra (degradê laranja → rosa da Legacy).
  int w = 0, h = 0, n = 0;
  if (!imagem.empty()) {
    if (stbi_uc* px = stbi_load(imagem.c_str(), &w, &h, &n, 4)) {
      logo_ = rhi::createTexture(ctx, px, uint32_t(w), uint32_t(h), srgb_);
      stbi_image_free(px);
    } else {
      LOG_WARN("Tela de carregamento: não abri %s", imagem.c_str());
    }
  }
  const unsigned char trilho[4] = {38, 41, 52, 255};
  trilho_ = rhi::createTexture(ctx, trilho, 1, 1, srgb_);
  std::vector<unsigned char> grad(256 * 4);
  for (int i = 0; i < 256; ++i) {
    const float t = float(i) / 255.0f;
    grad[i * 4 + 0] = (unsigned char)(255);
    grad[i * 4 + 1] = (unsigned char)(181 + (94 - 181) * t);
    grad[i * 4 + 2] = (unsigned char)(71 + (122 - 71) * t);
    grad[i * 4 + 3] = 255;
  }
  barra_ = rhi::createTexture(ctx, grad.data(), 256, 1, srgb_);
  ctx.immediateSubmit([&](VkCommandBuffer cmd) {
    for (rhi::Image* im : {&logo_, &trilho_, &barra_})
      if (im->handle)
        barreira(cmd, im->handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, im->mipLevels);
  });
}

void TelaCarregamento::desenhar(float fracao) {
  if (!ctx_) return;
  glfwPollEvents();  // a janela continua respondendo (mover, minimizar) durante a carga
  int fw = 0, fh = 0;
  glfwGetFramebufferSize(janela_, &fw, &fh);
  if (fw == 0 || fh == 0) return;  // minimizada
  if (uint32_t(fw) != sc_->extent.width || uint32_t(fh) != sc_->extent.height) {
    vkDeviceWaitIdle(ctx_->device);
    sc_->create(*ctx_, surface_, {uint32_t(fw), uint32_t(fh)}, vsync_);
  }
  uint32_t idx = 0;
  VkResult r = vkAcquireNextImageKHR(ctx_->device, sc_->handle, 100'000'000ull, VK_NULL_HANDLE, fence_, &idx);
  if (r == VK_TIMEOUT || r == VK_NOT_READY) return;
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    vkDeviceWaitIdle(ctx_->device);
    sc_->create(*ctx_, surface_, {uint32_t(fw), uint32_t(fh)}, vsync_);
    return;
  }
  if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
  vkWaitForFences(ctx_->device, 1, &fence_, VK_TRUE, UINT64_MAX);
  vkResetFences(ctx_->device, 1, &fence_);

  const VkImage tela = sc_->images[idx];
  const int W = int(sc_->extent.width), H = int(sc_->extent.height);
  ctx_->immediateSubmit([&](VkCommandBuffer cmd) {
    barreira(cmd, tela, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    // Fundo #0b0c10 (o mesmo do site e do launcher): numa tela sRGB o valor da limpeza é linear.
    VkClearColorValue fundo = srgb_ ? VkClearColorValue{{0.0033f, 0.0037f, 0.0052f, 1.0f}}
                                    : VkClearColorValue{{0.043f, 0.047f, 0.063f, 1.0f}};
    VkImageSubresourceRange tudo{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, tela, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &fundo, 1, &tudo);
    if (blit_) {
      int baseY = H / 2;
      if (logo_.handle) {  // imagem do produto: até 46% da largura e 46% da altura, sem distorcer
        const float lw = float(logo_.extent.width), lh = float(logo_.extent.height);
        const float s = std::min(W * 0.46f / lw, H * 0.46f / lh);
        const int dw = int(lw * s), dh = int(lh * s);
        const int x0 = (W - dw) / 2, y0 = H / 2 - dh / 2 - H / 20;
        copiar(cmd, logo_, 0, int(logo_.extent.width), tela, x0, y0, x0 + dw, y0 + dh);
        baseY = y0 + dh;
      }
      const int bw = std::max(120, W / 4), bh = std::max(3, H / 200);
      const int bx = (W - bw) / 2, by = std::min(H - bh - 8, baseY + H / 22);
      copiar(cmd, trilho_, 0, 1, tela, bx, by, bx + bw, by + bh);
      const float f = std::clamp(fracao, 0.0f, 1.0f);
      const int cheio = int(bw * f);
      copiar(cmd, barra_, 0, std::max(1, int(256 * f)), tela, bx, by, bx + cheio, by + bh);  // o degradê vai se revelando
    }
    barreira(cmd, tela, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
             0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  });
  // immediateSubmit já esperou a fila: apresenta sem semáforo.
  VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  pi.swapchainCount = 1;
  pi.pSwapchains = &sc_->handle;
  pi.pImageIndices = &idx;
  vkQueuePresentKHR(ctx_->queue, &pi);
}

void TelaCarregamento::encerrar() {
  if (!ctx_) return;
  vkDeviceWaitIdle(ctx_->device);
  for (rhi::Image* im : {&logo_, &trilho_, &barra_})
    if (im->handle) rhi::destroyImage(*ctx_, *im);
  if (fence_) vkDestroyFence(ctx_->device, fence_, nullptr);
  fence_ = VK_NULL_HANDLE;
  ctx_ = nullptr;
}
