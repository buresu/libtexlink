#pragma once

#include <vulkan/vulkan.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_vk_image texlink_vk_image_t;
typedef struct texlink_vk_buffer texlink_vk_buffer_t;

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
  VkMemoryPropertyFlags memory_flags;
} texlink_vk_image_frame_desc_t;

typedef struct {
  VkDevice device;
  VkPhysicalDeviceMemoryProperties memory_properties;
  uint64_t size;
  VkBufferUsageFlags usage;
  VkMemoryPropertyFlags memory_flags;
} texlink_vk_buffer_frame_desc_t;

typedef struct {
  VkDevice device;
  VkImage image;
  VkDeviceMemory memory;
  texlink_native_handle_t handle;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint64_t size;
  uint64_t modifier;
} texlink_vk_wrap_image_desc_t;

typedef struct {
  VkDevice device;
  VkBuffer buffer;
  VkDeviceMemory memory;
  texlink_native_handle_t handle;
  uint64_t size;
} texlink_vk_wrap_buffer_desc_t;

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

texlink_vk_buffer_t *
texlink_vk_buffer_frame_create(const texlink_vk_buffer_frame_desc_t *desc);
VkBuffer texlink_vk_buffer_handle(texlink_vk_buffer_t *buffer);
VkDeviceMemory texlink_vk_buffer_memory(texlink_vk_buffer_t *buffer);
texlink_frame_t *texlink_vk_buffer_frame_frame(texlink_vk_buffer_t *buffer);
void texlink_vk_buffer_destroy(texlink_vk_buffer_t *buffer);

texlink_frame_t *
texlink_vk_frame_wrap_buffer(const texlink_vk_wrap_buffer_desc_t *desc);

#ifdef __cplusplus
}
#endif
