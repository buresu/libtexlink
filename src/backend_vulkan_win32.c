#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "backend_vulkan_internal.h"

#include <stdlib.h>
#include <string.h>

static texlink_native_handle_type_t vk_default_memory_handle_type(void) {
  return TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE;
}

static VkExternalMemoryHandleTypeFlagBits
vk_external_memory_handle_type(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE:
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  case TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE:
    return VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
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
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = {meta.width, meta.height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
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
  VkImportMemoryWin32HandleInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
      .pNext = &dedicated,
      .handleType = vk_handle_type,
      .handle = (HANDLE)handle.value.ptr,
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
  texlink_native_handle_close(&handle);

  result = vkBindImageMemory(device, image->image, image->memory, 0);
  if (result != VK_SUCCESS)
    goto err;

  return image;

err:
  if (handle.value.ptr)
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

  texlink_vk_image_t *image = import_frame_with_handle_type(
      device, memory_properties, frame, format, usage, initial_layout,
      native_type, vk_handle_type);
  if (!image && native_type == TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE) {
    image = import_frame_with_handle_type(
        device, memory_properties, frame, format, usage, initial_layout,
        native_type, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT);
  }
  return image;
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
  VkFormat vk_format = texlink_vk_format(format);
  if (vk_format == VK_FORMAT_UNDEFINED)
    return NULL;

  texlink_vk_image_t *image = calloc(1, sizeof(*image));
  if (!image)
    return NULL;
  image->device = desc->device;

  VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = vk_format,
      .extent = {desc->width, desc->height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = desc->usage ? desc->usage
                           : (VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT),
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = desc->initial_layout,
  };
  if (vkCreateImage(desc->device, &image_info, NULL, &image->image) !=
      VK_SUCCESS)
    goto err;

  VkMemoryRequirements memory_requirements;
  vkGetImageMemoryRequirements(desc->device, image->image, &memory_requirements);

  VkExportMemoryAllocateInfo export_info = {
      .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
  };
  VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .pNext = &export_info,
      .image = image->image,
  };
  VkMemoryAllocateInfo allocate_info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &dedicated,
      .allocationSize = memory_requirements.size,
      .memoryTypeIndex = find_memory_type(&desc->memory_properties,
                                          memory_requirements.memoryTypeBits, 0),
  };
  if (allocate_info.memoryTypeIndex == UINT32_MAX)
    goto err;
  if (vkAllocateMemory(desc->device, &allocate_info, NULL, &image->memory) !=
      VK_SUCCESS)
    goto err;
  if (vkBindImageMemory(desc->device, image->image, image->memory, 0) !=
      VK_SUCCESS)
    goto err;

  image->frame = texlink_vk_frame_wrap_image(&(texlink_vk_wrap_image_desc_t){
      .device = desc->device,
      .image = image->image,
      .memory = image->memory,
      .width = desc->width,
      .height = desc->height,
      .format = format,
      .size = memory_requirements.size,
  });
  if (!image->frame)
    goto err;
  image->owns_frame = 1;
  return image;

err:
  destroy_vk_image(image);
  free(image);
  return NULL;
}

texlink_frame_t *
texlink_vk_frame_wrap_image(const texlink_vk_wrap_image_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;
  texlink_native_handle_t handle = desc->handle;
  if (handle.handle_type == TEXLINK_NATIVE_HANDLE_UNKNOWN) {
    if (!desc->device || !desc->memory)
      return NULL;

    PFN_vkGetMemoryWin32HandleKHR get_memory_handle =
        (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(
            desc->device, "vkGetMemoryWin32HandleKHR");
    if (!get_memory_handle)
      return NULL;

    VkMemoryGetWin32HandleInfoKHR handle_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
        .memory = desc->memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
    };
    HANDLE win32_handle = NULL;
    if (get_memory_handle(desc->device, &handle_info, &win32_handle) !=
            VK_SUCCESS ||
        !win32_handle)
      return NULL;

    handle.handle_type = TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE;
    handle.flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED;
    handle.value.ptr = win32_handle;
  }

  if (!vk_external_memory_handle_type(handle.handle_type))
    return NULL;
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
