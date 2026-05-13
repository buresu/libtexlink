#include <texlink_vulkan.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct texlink_vk_image {
  VkDevice device;
  VkImage image;
  VkDeviceMemory memory;
  texlink_frame_t *frame;
  int owns_frame;
};

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

static uint32_t
find_memory_type(const VkPhysicalDeviceMemoryProperties *memory_properties,
                 uint32_t type_bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < memory_properties->memoryTypeCount; i++) {
    if ((type_bits & (1u << i)) &&
        (memory_properties->memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  }
  return UINT32_MAX;
}

static void destroy_vk_image(texlink_vk_image_t *image) {
  if (!image || !image->device)
    return;
  if (image->image)
    vkDestroyImage(image->device, image->image, NULL);
  if (image->memory)
    vkFreeMemory(image->device, image->memory, NULL);
  image->image = VK_NULL_HANDLE;
  image->memory = VK_NULL_HANDLE;
}

static texlink_vk_image_t *
import_frame(VkDevice device,
             const VkPhysicalDeviceMemoryProperties *memory_properties,
             texlink_frame_t *frame, VkFormat format, VkImageUsageFlags usage,
             VkImageLayout initial_layout) {
  if (!device || !memory_properties || !frame)
    return NULL;

  texlink_meta_t meta = texlink_frame_meta(frame);
  if (format == VK_FORMAT_UNDEFINED)
    format = texlink_vk_format(meta.format);
  if (format == VK_FORMAT_UNDEFINED)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_dup_native_handle(frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
                                      &handle) != 0)
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
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
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
      .tiling = VK_IMAGE_TILING_LINEAR,
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
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
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

texlink_vk_image_t *
texlink_vk_image_import(const texlink_vk_import_desc_t *desc) {
  if (!desc)
    return NULL;
  return import_frame(desc->device, &desc->memory_properties, desc->frame,
                      desc->format, desc->usage, desc->initial_layout);
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

texlink_vk_image_t *
texlink_vk_image_frame_create(const texlink_vk_image_frame_desc_t *desc) {
  if (!desc || !desc->device || desc->width == 0 || desc->height == 0)
    return NULL;

  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  texlink_frame_t *frame = texlink_frame_create(&(texlink_frame_desc_t){
      .version = 1,
      .type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
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

texlink_frame_t *texlink_vk_image_frame_frame(texlink_vk_image_t *image) {
  return image ? image->frame : NULL;
}

texlink_frame_t *
texlink_vk_frame_wrap_image(const texlink_vk_wrap_image_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;
  if (desc->size > UINT32_MAX)
    return NULL;

  int fd = desc->dma_buf_fd;
  uint32_t flags = desc->flags;
  if (fd < 0) {
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
    if (get_memory_fd(desc->device, &fd_info, &fd) != VK_SUCCESS || fd < 0)
      return NULL;
    flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED;
  }

  texlink_native_handle_t handle = {
      .version = 1,
      .type = TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
      .flags = flags ? flags : TEXLINK_NATIVE_HANDLE_FLAG_BORROWED,
      .value.fd = fd,
  };
  return texlink_frame_create_from_native_handle(&(texlink_frame_native_desc_t){
      .version = 1,
      .type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .depth = 1,
      .format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888,
      .stride = desc->stride,
      .modifier = desc->modifier,
      .size = desc->size,
      .backend = TEXLINK_BACKEND_VULKAN,
      .handle = handle,
  });
}
