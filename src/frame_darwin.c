#include "texlink_internal.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t format_bytes_per_pixel(uint32_t format) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return 1;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    return 4;
  default:
    return 0;
  }
}

static uint32_t infer_stride(uint32_t width, uint32_t format) {
  uint32_t bpp = format_bytes_per_pixel(format);
  return (width != 0 && bpp != 0) ? width * bpp : 0;
}

static uint32_t iosurface_pixel_format(uint32_t format) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return 'L008';
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    return 'RGBA';
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
  default:
    return 'BGRA';
  }
}

static void frame_set_iosurface(texlink_frame_t *frame, IOSurfaceRef surface,
                                uint32_t width, uint32_t height,
                                uint32_t format,
                                texlink_frame_type_t frame_type,
                                texlink_backend_t backend_type) {
  IOSurfaceID surface_id = IOSurfaceGetID(surface);
  size_t stride = IOSurfaceGetBytesPerRow(surface);
  size_t size = IOSurfaceGetAllocSize(surface);

  frame->iosurface = surface;
  frame->dma_fd = -1;
  frame->sync_fd = -1;
  frame->index = -1;
  frame->drm_fd = -1;
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->size = size;
  frame->handle.handle_type = TEXLINK_NATIVE_HANDLE_IOSURFACE_ID;
  frame->handle.owned = 0;
  frame->handle.value.ptr = (void *)(uintptr_t)surface_id;
  frame->meta.backend_type = (uint32_t)backend_type;
  frame->meta.handle_type = TEXLINK_NATIVE_HANDLE_IOSURFACE_ID;
  frame->meta.frame_type = frame_type;
  frame->meta.width = width;
  frame->meta.height = height;
  frame->meta.depth = 1;
  frame->meta.format = format;
  frame->meta.stride = (uint32_t)stride;
  frame->meta.modifier = surface_id;
  frame->meta.size = size;
}

static int frame_ensure_iosurface(texlink_frame_t *frame) {
  if (!frame)
    return -EINVAL;
  if (frame->iosurface)
    return 0;
  if (frame->handle.handle_type != TEXLINK_NATIVE_HANDLE_IOSURFACE_ID)
    return -EINVAL;

  IOSurfaceID surface_id = (IOSurfaceID)(uintptr_t)frame->handle.value.ptr;
  if (surface_id == 0 && frame->meta.modifier != 0)
    surface_id = (IOSurfaceID)frame->meta.modifier;
  if (surface_id == 0)
    return -ENOENT;

  IOSurfaceRef surface = IOSurfaceLookup(surface_id);
  if (!surface)
    return -ENOENT;
  frame->iosurface = surface;
  return 0;
}

static texlink_frame_t *alloc_iosurface(uint32_t width, uint32_t height,
                                        uint32_t format,
                                        texlink_frame_type_t frame_type) {
  uint32_t bpp = format_bytes_per_pixel(format);
  if (width == 0 || height == 0 || bpp == 0)
    return NULL;

  /*
   * libtexlink transports IOSurfaceID values over its existing socket protocol.
   * That requires a globally lookupable IOSurface; callers that need a tighter
   * security model should add a Mach-port transport and use
   * IOSurfaceLookupFromMachPort instead.
   */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight,
                        kIOSurfaceBytesPerElement, kIOSurfacePixelFormat,
                        kIOSurfaceIsGlobal};
#pragma clang diagnostic pop
  uint32_t pixel_format = iosurface_pixel_format(format);
  const void *values[] = {
      CFNumberCreate(NULL, kCFNumberSInt32Type, &width),
      CFNumberCreate(NULL, kCFNumberSInt32Type, &height),
      CFNumberCreate(NULL, kCFNumberSInt32Type, &bpp),
      CFNumberCreate(NULL, kCFNumberSInt32Type, &pixel_format),
      kCFBooleanTrue,
  };
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    if (!values[i]) {
      for (size_t j = 0; j < i; j++)
        CFRelease(values[j]);
      return NULL;
    }
  }

  CFDictionaryRef props = CFDictionaryCreate(
      NULL, keys, values, sizeof(keys) / sizeof(keys[0]),
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    CFRelease(values[i]);
  if (!props)
    return NULL;

  IOSurfaceRef surface = IOSurfaceCreate(props);
  CFRelease(props);
  if (!surface)
    return NULL;

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    CFRelease(surface);
    return NULL;
  }
  frame_set_iosurface(frame, surface, width, height, format, frame_type,
                      TEXLINK_BACKEND_UNKNOWN);
  return frame;
}

texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc) {
  if (!desc)
    return NULL;

  uint32_t width = desc->width;
  uint32_t height = desc->height ? desc->height : 1;
  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;

  switch (desc->frame_type) {
  case TEXLINK_FRAME_TYPE_RAW:
  case TEXLINK_FRAME_TYPE_VERTEX_BUFFER:
  case TEXLINK_FRAME_TYPE_COMPUTE_BUFFER: {
    uint64_t size = desc->size;
    if (size == 0) {
      uint32_t stride = desc->stride ? desc->stride : infer_stride(width, format);
      size = (uint64_t)stride * height;
    }
    if (size == 0 || size > UINT32_MAX)
      return NULL;
    return alloc_iosurface((uint32_t)size, 1, TEXLINK_FRAME_FORMAT_R8,
                           desc->frame_type);
  }
  case TEXLINK_FRAME_TYPE_TEXTURE_2D:
    return alloc_iosurface(width, height, format, desc->frame_type);
  default:
    return NULL;
  }
}

texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc) {
  if (!desc ||
      desc->handle.handle_type != TEXLINK_NATIVE_HANDLE_IOSURFACE_ID)
    return NULL;

  IOSurfaceID surface_id = (IOSurfaceID)(uintptr_t)desc->handle.value.ptr;
  if (surface_id == 0 && desc->modifier != 0)
    surface_id = (IOSurfaceID)desc->modifier;
  if (surface_id == 0)
    return NULL;

  IOSurfaceRef surface = IOSurfaceLookup(surface_id);
  if (!surface)
    return NULL;

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    CFRelease(surface);
    return NULL;
  }

  uint32_t width =
      desc->width ? desc->width : (uint32_t)IOSurfaceGetWidth(surface);
  uint32_t height =
      desc->height ? desc->height : (uint32_t)IOSurfaceGetHeight(surface);
  uint32_t format =
      desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  frame_set_iosurface(frame, surface, width, height, format, desc->frame_type,
                      desc->backend_type);
  if (desc->stride)
    frame->meta.stride = desc->stride;
  if (desc->size) {
    frame->meta.size = desc->size;
    frame->size = (size_t)desc->size;
  }
  return frame;
}

void texlink_frame_destroy(texlink_frame_t *frame) {
  if (!frame)
    return;
  if (frame->map_base)
    IOSurfaceUnlock(frame->iosurface, 0, NULL);
  if (frame->iosurface)
    CFRelease(frame->iosurface);
  free(frame);
}

int texlink_frame_should_flip_y(texlink_backend_t producer,
                                texlink_backend_t consumer) {
  if (producer == TEXLINK_BACKEND_UNKNOWN ||
      consumer == TEXLINK_BACKEND_UNKNOWN || producer == consumer)
    return 0;
  if (producer == TEXLINK_BACKEND_CGL && consumer == TEXLINK_BACKEND_METAL)
    return 1;
  if (producer == TEXLINK_BACKEND_METAL && consumer == TEXLINK_BACKEND_CGL)
    return 1;
  return 0;
}

texlink_meta_t texlink_frame_meta(texlink_frame_t *frame) {
  if (!frame) {
    texlink_meta_t zero = {0};
    return zero;
  }
  return frame->meta;
}

int texlink_frame_index(texlink_frame_t *frame) {
  return frame ? frame->index : -1;
}

int texlink_frame_get_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  if (!frame || !out_handle || type != TEXLINK_NATIVE_HANDLE_IOSURFACE_ID)
    return -EINVAL;
  if (!frame->iosurface && frame->handle.handle_type != type)
    return -ENOENT;
  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->handle_type = type;
  out_handle->owned = 0;
  out_handle->value.ptr =
      frame->iosurface
          ? (void *)(uintptr_t)IOSurfaceGetID(frame->iosurface)
          : frame->handle.value.ptr;
  return 0;
}

int texlink_frame_dup_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  return texlink_frame_get_native_handle(frame, type, out_handle);
}

int texlink_frame_set_sync_native_handle(texlink_frame_t *frame,
                                         const texlink_native_handle_t *handle,
                                         uint64_t value) {
  (void)handle;
  if (!frame)
    return -EINVAL;
  frame->meta.sync_value = value;
  return 0;
}

int texlink_frame_get_sync_native_handle(texlink_frame_t *frame,
                                         texlink_native_handle_type_t type,
                                         texlink_native_handle_t *out_handle,
                                         uint64_t *out_value) {
  (void)frame;
  (void)type;
  (void)out_handle;
  (void)out_value;
  return -ENOENT;
}

int texlink_frame_set_sync_value(texlink_frame_t *frame, uint64_t value) {
  if (!frame)
    return -EINVAL;
  frame->meta.sync_value = value;
  return 0;
}

uint64_t texlink_frame_sync_value(texlink_frame_t *frame) {
  return frame ? frame->meta.sync_value : 0;
}

int texlink_native_handle_close(texlink_native_handle_t *handle) {
  if (!handle)
    return -EINVAL;
  if (handle->handle_type != TEXLINK_NATIVE_HANDLE_IOSURFACE_ID)
    return -ENOTSUP;
  memset(handle, 0, sizeof(*handle));
  return 0;
}

int texlink_frame_map(texlink_frame_t *frame, const texlink_map_desc_t *desc,
                      texlink_mapping_t *out_mapping) {
  if (!frame || !out_mapping)
    return -EINVAL;
  int ret = frame_ensure_iosurface(frame);
  if (ret != 0)
    return ret;
  if (frame->map_base)
  {
    uint64_t offset = desc ? desc->offset : frame->map_offset;
    uint64_t size = desc && desc->size ? desc->size : frame->map_length;
    if (size == 0)
      size = frame->meta.size - offset;
    if (offset > frame->meta.size || size > frame->meta.size - offset)
      return -EINVAL;
    out_mapping->data = (uint8_t *)frame->map_base + offset;
    out_mapping->size = size;
    out_mapping->stride = frame->meta.stride;
    return 0;
  }
  uint64_t offset = desc ? desc->offset : 0;
  uint64_t size = desc && desc->size ? desc->size : frame->meta.size;
  if (offset > frame->meta.size || size > frame->meta.size - offset)
    return -EINVAL;
  if (IOSurfaceLock(frame->iosurface, 0, NULL) != kIOReturnSuccess)
    return -EIO;
  void *base = IOSurfaceGetBaseAddress(frame->iosurface);
  if (!base) {
    IOSurfaceUnlock(frame->iosurface, 0, NULL);
    return -EIO;
  }
  frame->map_base = base;
  frame->map_ptr = (uint8_t *)base + offset;
  frame->map_size = (size_t)frame->meta.size;
  frame->map_offset = offset;
  frame->map_length = size;
  out_mapping->data = frame->map_ptr;
  out_mapping->size = size;
  out_mapping->stride = frame->meta.stride;
  return 0;
}

int texlink_frame_unmap(texlink_frame_t *frame) {
  if (!frame || !frame->map_base || !frame->iosurface)
    return -EINVAL;
  IOSurfaceUnlock(frame->iosurface, 0, NULL);
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->map_size = 0;
  frame->map_offset = 0;
  frame->map_length = 0;
  return 0;
}

int texlink_frame_is_mapped(texlink_frame_t *frame) {
  return frame && frame->map_base;
}

int texlink_frame_cpu_begin(texlink_frame_t *frame,
                            const texlink_map_desc_t *desc) {
  if (!frame)
    return -EINVAL;
  if (frame->map_base) {
    frame->active_access = 0;
    return 0;
  }
  texlink_mapping_t mapping;
  int ret = texlink_frame_map(frame, desc, &mapping);
  if (ret == 0)
    frame->active_access = 1;
  return ret;
}

int texlink_frame_cpu_end(texlink_frame_t *frame,
                          const texlink_map_desc_t *desc) {
  (void)desc;
  if (!frame)
    return -EINVAL;
  if (!frame->active_access)
    return 0;
  frame->active_access = 0;
  return texlink_frame_unmap(frame);
}
