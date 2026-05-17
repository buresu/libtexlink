#include "backend_vulkan_internal.h"

#include <stdlib.h>

VkFormat texlink_vk_format(uint32_t texlink_format) {
  switch (texlink_format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return VK_FORMAT_R8_UNORM;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    return VK_FORMAT_R8G8B8A8_UNORM;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

uint32_t
find_memory_type(const VkPhysicalDeviceMemoryProperties *memory_properties,
                 uint32_t type_bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < memory_properties->memoryTypeCount; i++) {
    if ((type_bits & (1u << i)) &&
        (memory_properties->memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  }
  return UINT32_MAX;
}

void destroy_vk_image(texlink_vk_image_t *image) {
  if (!image || !image->device)
    return;
  if (image->image)
    vkDestroyImage(image->device, image->image, NULL);
  if (image->memory)
    vkFreeMemory(image->device, image->memory, NULL);
  image->image = VK_NULL_HANDLE;
  image->memory = VK_NULL_HANDLE;
}

VkImage texlink_vk_image_handle(texlink_vk_image_t *image) {
  return image ? image->image : VK_NULL_HANDLE;
}

VkDeviceMemory texlink_vk_image_memory(texlink_vk_image_t *image) {
  return image ? image->memory : VK_NULL_HANDLE;
}

void texlink_vk_image_destroy(texlink_vk_image_t *image) {
  if (!image)
    return;
  destroy_vk_image(image);
  if (image->owns_frame)
    texlink_frame_destroy(image->frame);
  free(image);
}

texlink_frame_t *texlink_vk_image_frame_frame(texlink_vk_image_t *image) {
  return image ? image->frame : NULL;
}
