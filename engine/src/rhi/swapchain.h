#pragma once
#include <vector>

#include "rhi/context.h"

namespace rhi {

class Swapchain {
 public:
  void create(Context& ctx, VkSurfaceKHR surface, VkExtent2D desired, bool vsync);
  void destroy(Context& ctx);

  VkSwapchainKHR handle = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkSemaphore> renderFinished;  // um por imagem (sinalizado antes do present)
};

}  // namespace rhi
