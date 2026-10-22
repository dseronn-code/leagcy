#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "core/log.h"

#define VK_CHECK(expr)                                                              \
  do {                                                                              \
    VkResult vk_check_result_ = (expr);                                             \
    if (vk_check_result_ != VK_SUCCESS)                                             \
      FATAL("%s falhou (VkResult %d) em %s:%d", #expr, vk_check_result_, __FILE__, __LINE__); \
  } while (0)
