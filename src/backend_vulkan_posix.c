#include "backend_vulkan_internal.h"

#include <stdlib.h>
#include <string.h>

static texlink_native_handle_type_t vk_default_memory_handle_type(void) {
  return TEXLINK_NATIVE_HANDLE_DMA_BUF_FD;
}

static VkExternalMemoryHandleTypeFlagBits
vk_external_memory_handle_type(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_DMA_BUF_FD:
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  default:
    return 0;
  }
}

static texlink_vk_image_t *import_frame_with_handle_type(
    VkDevice device, const VkPhysicalDeviceMemoryProperties *memory_properties,
    texlink_frame_t *frame, VkFormat format, VkImageUsageFlags usage,
    VkImageLayout initial_layout, texlink_native_handle_type_t native_type,
    VkExternalMemoryHandleTypeFlagBits vk_handle_type) {
  if (!device || !memory_properties || !frame)
    return NULL;

  texlink_meta_t meta = texlink_frame_meta(frame);
  if (format == VK_FORMAT_UNDEFINED)
    format = texlink_vk_format(meta.format);
  if (format == VK_FORMAT_UNDEFINED)
    return NULL;

  if (!vk_handle_type)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_dup_native_handle(frame, native_type, &handle) != 0)
    return NULL;

  texlink_vk_image_t *image = calloc(1, sizeof(*image));
  if (!image) {
    texlink_native_handle_close(&handle);
    return NULL;
  }
  image->device = device;
  image->frame = frame;

  VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = vk_handle_type,
  };
  VkSubresourceLayout plane_layout = {
      .offset = 0,
      .rowPitch = meta.stride,
  };
  VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
      .sType =
          VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = meta.modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane_layout,
  };
  if (meta.modifier != TEXLINK_DRM_FORMAT_MOD_INVALID && meta.stride != 0)
    external.pNext = &modifier;

  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {meta.width, meta.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = external.pNext ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT
                               : VK_IMAGE_TILING_LINEAR,
      .usage = usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = initial_layout,
  };
  if (image_info.usage == 0)
    image_info.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

  VkResult result = vkCreateImage(device, &image_info, NULL, &image->image);
  if (result != VK_SUCCESS)
    goto err;

  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(device, image->image, &memory_requirements);

  VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image->image,
  };
  VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = vk_handle_type,
      .fd = handle.value.fd,
  };
  VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = find_memory_type(
          memory_properties, memory_requirements.memoryTypeBits, 0),
  };
  if (allocate_info.memoryTypeIndex == UINT32_MAX)
    goto err;

  result = vkAllocateMemory(device, &allocate_info, NULL, &image->memory);
  if (result != VK_SUCCESS)
    goto err;
  // fd ownership is transferred to Vulkan on successful vkAllocateMemory
  memset(&handle, 0, sizeof(handle));
  handle.value.fd = -1;

  result = vkBindImageMemory(device, image->image, image->memory, 0);
  if (result != VK_SUCCESS)
    goto err;

  return image;

err:
  if (handle.value.fd >= 0)
    texlink_native_handle_close(&handle);
  destroy_vk_image(image);
  free(image);
  return NULL;
}

static texlink_vk_image_t *
import_frame(VkDevice device,
             const VkPhysicalDeviceMemoryProperties *memory_properties,
             texlink_frame_t *frame, VkFormat format, VkImageUsageFlags usage,
             VkImageLayout initial_layout) {
  if (!frame)
    return NULL;
  texlink_meta_t meta = texlink_frame_meta(frame);
  texlink_native_handle_type_t native_type =
      meta.handle_type ? (texlink_native_handle_type_t)meta.handle_type
                       : vk_default_memory_handle_type();
  VkExternalMemoryHandleTypeFlagBits vk_handle_type =
      vk_external_memory_handle_type(native_type);

  return import_frame_with_handle_type(device, memory_properties, frame, format,
                                       usage, initial_layout, native_type,
                                       vk_handle_type);
}

texlink_vk_image_t *
texlink_vk_image_import(const texlink_vk_import_desc_t *desc) {
  if (!desc)
    return NULL;
  return import_frame(desc->device, &desc->memory_properties, desc->frame,
                      desc->format, desc->usage, desc->initial_layout);
}

texlink_vk_image_t *
texlink_vk_image_frame_create(const texlink_vk_image_frame_desc_t *desc) {
  if (!desc || !desc->device || desc->width == 0 || desc->height == 0)
    return NULL;

  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  texlink_frame_t *frame = texlink_frame_create(&(texlink_frame_desc_t){
      .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .format = format,
  });
  if (!frame)
    return NULL;

  texlink_vk_image_t *image = import_frame(
      desc->device, &desc->memory_properties, frame, texlink_vk_format(format),
      desc->usage, desc->initial_layout);
  if (!image) {
    texlink_frame_destroy(frame);
    return NULL;
  }
  image->owns_frame = 1;
  return image;
}

texlink_frame_t *
texlink_vk_frame_wrap_image(const texlink_vk_wrap_image_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;
  texlink_native_handle_t handle = desc->handle;
  if (handle.handle_type == TEXLINK_NATIVE_HANDLE_UNKNOWN) {
    if (!desc->device || !desc->memory)
      return NULL;

    PFN_vkGetMemoryFdKHR get_memory_fd =
        (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(desc->device,
                                                  "vkGetMemoryFdKHR");
    if (!get_memory_fd)
      return NULL;

    VkMemoryGetFdInfoKHR fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .memory = desc->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    if (get_memory_fd(desc->device, &fd_info, &fd) != VK_SUCCESS || fd < 0)
      return NULL;
    handle = (texlink_native_handle_t){
        .handle_type = TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
        .flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED,
        .value.fd = fd,
    };
  } else if (handle.handle_type != TEXLINK_NATIVE_HANDLE_DMA_BUF_FD) {
    return NULL;
  }

  if (!handle.flags)
    handle.flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  return texlink_frame_create_from_native_handle(&(texlink_frame_native_desc_t){
      .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .depth = 1,
      .format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888,
      .stride = desc->stride,
      .modifier = desc->modifier,
      .size = desc->size,
      .backend_type = TEXLINK_BACKEND_VULKAN,
      .handle = handle,
  });
}
