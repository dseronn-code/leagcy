#include "rhi/swapchain.h"

#include <algorithm>

namespace rhi {

void Swapchain::create(Context& ctx, VkSurfaceKHR surface, VkExtent2D desired, bool vsync) {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.gpu, surface, &caps));

  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.gpu, surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.gpu, surface, &n, formats.data());
  VkSurfaceFormatKHR chosen = formats[0];
  for (auto& f : formats) {
    if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
  }
  format = chosen.format;

  vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.gpu, surface, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.gpu, surface, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // sempre disponível
  if (!vsync) {
    for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
    if (mode == VK_PRESENT_MODE_FIFO_KHR)
      for (auto m : modes) if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
  }

  if (caps.currentExtent.width != UINT32_MAX) extent = caps.currentExtent;
  else extent = {std::clamp(desired.width, caps.minImageExtent.width, caps.maxImageExtent.width),
                 std::clamp(desired.height, caps.minImageExtent.height, caps.maxImageExtent.height)};

  uint32_t count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0) count = std::min(count, caps.maxImageCount);

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface;
  ci.minImageCount = count;
  ci.imageFormat = chosen.format;
  ci.imageColorSpace = chosen.colorSpace;
  ci.imageExtent = extent;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = mode;
  ci.clipped = VK_TRUE;
  ci.oldSwapchain = handle;
  VkSwapchainKHR newHandle;
  VK_CHECK(vkCreateSwapchainKHR(ctx.device, &ci, nullptr, &newHandle));
  destroy(ctx);
  handle = newHandle;

  vkGetSwapchainImagesKHR(ctx.device, handle, &n, nullptr);
  images.resize(n);
  vkGetSwapchainImagesKHR(ctx.device, handle, &n, images.data());
  views.resize(n);
  renderFinished.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images[i];
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device, &vi, nullptr, &views[i]));
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx.device, &si, nullptr, &renderFinished[i]));
  }
}

void Swapchain::destroy(Context& ctx) {
  for (auto v : views) vkDestroyImageView(ctx.device, v, nullptr);
  for (auto s : renderFinished) vkDestroySemaphore(ctx.device, s, nullptr);
  views.clear();
  renderFinished.clear();
  images.clear();
  if (handle) vkDestroySwapchainKHR(ctx.device, handle, nullptr);
  handle = VK_NULL_HANDLE;
}

}  // namespace rhi
