#pragma once

#include <texlink_vulkan.h>

#define TEXLINK_DRM_FORMAT_MOD_INVALID ((1ULL << 56) - 1)

struct texlink_vk_image {
  VkDevice device;
  VkImage image;
  VkDeviceMemory memory;
  texlink_frame_t *frame;
  int owns_frame;
};

uint32_t find_memory_type(
    const VkPhysicalDeviceMemoryProperties *memory_properties,
    uint32_t type_bits, VkMemoryPropertyFlags flags);
void destroy_vk_image(texlink_vk_image_t *image);
