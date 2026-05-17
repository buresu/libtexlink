#pragma once

#include <vulkan/vulkan.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_vk_image texlink_vk_image_t;

typedef struct {
  VkDevice device;
  VkPhysicalDeviceMemoryProperties memory_properties;
  texlink_frame_t *frame;
  VkFormat format;
  VkImageUsageFlags usage;
  VkImageLayout initial_layout;
} texlink_vk_import_desc_t;

typedef struct {
  VkDevice device;
  VkPhysicalDeviceMemoryProperties memory_properties;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  VkImageUsageFlags usage;
  VkImageLayout initial_layout;
} texlink_vk_image_frame_desc_t;

typedef struct {
  VkDevice device;
  VkImage image;
  VkDeviceMemory memory;
  int dma_buf_fd;
  texlink_native_handle_t handle;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint64_t size;
  uint64_t modifier;
  uint32_t flags;
} texlink_vk_wrap_image_desc_t;

VkFormat texlink_vk_format(uint32_t texlink_format);

texlink_vk_image_t *
texlink_vk_image_import(const texlink_vk_import_desc_t *desc);
VkImage texlink_vk_image_handle(texlink_vk_image_t *image);
VkDeviceMemory texlink_vk_image_memory(texlink_vk_image_t *image);
void texlink_vk_image_destroy(texlink_vk_image_t *image);

texlink_vk_image_t *
texlink_vk_image_frame_create(const texlink_vk_image_frame_desc_t *desc);
texlink_frame_t *texlink_vk_image_frame_frame(texlink_vk_image_t *image);

texlink_frame_t *
texlink_vk_frame_wrap_image(const texlink_vk_wrap_image_desc_t *desc);

#ifdef __cplusplus
}
#endif
