#include "texlink_internal.h"

#include <errno.h>
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
  if (width == 0 || bpp == 0)
    return 0;
  return width * bpp;
}

static int is_win32_handle_type(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE:
    return 1;
  default:
    return 0;
  }
}

static int is_shared_win32_memory_handle(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE:
  case TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE:
    return 1;
  default:
    return 0;
  }
}

static void frame_set_handle(texlink_frame_t *frame,
                             texlink_native_handle_type_t type, HANDLE handle,
                             uint32_t flags) {
  frame->handle.version = 1;
  frame->handle.type = type;
  frame->handle.flags = flags;
  frame->handle.value.ptr = handle;
  frame->win32_handle = handle;
  frame->meta.handle_type = (uint32_t)type;
  frame->dma_fd = -1;
}

static texlink_frame_t *alloc_mapping(size_t size, uint32_t w, uint32_t h,
                                      uint32_t stride, uint32_t format,
                                      texlink_frame_type_t type) {
  HANDLE mapping =
      CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                         (DWORD)(size >> 32), (DWORD)size, NULL);
  if (!mapping)
    return NULL;

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    CloseHandle(mapping);
    return NULL;
  }

  frame_set_handle(frame, TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE, mapping,
                   TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
  frame->sync_fd = -1;
  frame->sync_handle = NULL;
  frame->index = -1;
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->drm_fd = -1;
  frame->size = size;
  frame->meta.backend = TEXLINK_BACKEND_CPU;
  frame->meta.type = type;
  frame->meta.width = w;
  frame->meta.height = h;
  frame->meta.depth = 1;
  frame->meta.format = format;
  frame->meta.stride = stride ? stride : infer_stride(w, format);
  frame->meta.size = (uint32_t)size;
  return frame;
}

texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc) {
  if (!desc || desc->size > UINT32_MAX)
    return NULL;

  uint32_t h = desc->height ? desc->height : 1;
  uint32_t stride =
      desc->stride ? desc->stride : infer_stride(desc->width, desc->format);
  size_t size = desc->size ? (size_t)desc->size : (size_t)stride * h;
  if (size == 0)
    size = (size_t)desc->width * h;
  if (size == 0 || size > UINT32_MAX)
    return NULL;

  switch (desc->type) {
  case TEXLINK_FRAME_TYPE_RAW:
  case TEXLINK_FRAME_TYPE_VERTEX_BUFFER:
  case TEXLINK_FRAME_TYPE_COMPUTE_BUFFER:
    return alloc_mapping(size, desc->width ? desc->width : (uint32_t)size, h,
                         stride, desc->format, desc->type);
  default:
    return NULL;
  }
}

texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc) {
  if (!desc || !is_win32_handle_type(desc->handle.type) ||
      !desc->handle.value.ptr || desc->size > UINT32_MAX)
    return NULL;

  uint64_t size = desc->size;
  if (size == 0 && desc->stride != 0 && desc->height != 0)
    size = (uint64_t)desc->stride * desc->height;
  if (size > UINT32_MAX)
    return NULL;

  HANDLE handle = (HANDLE)desc->handle.value.ptr;
  if (!(desc->handle.flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED) &&
      desc->handle.type != TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE) {
    HANDLE dup_handle = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return NULL;
    handle = dup_handle;
  }

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    CloseHandle(handle);
    return NULL;
  }

  frame_set_handle(frame, desc->handle.type, handle,
                   desc->handle.type == TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE
                       ? desc->handle.flags
                       : TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
  frame->sync_fd = -1;
  frame->sync_handle = NULL;
  frame->index = -1;
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->drm_fd = -1;
  frame->size = (size_t)size;
  frame->meta.backend = (uint32_t)desc->backend;
  frame->meta.type = desc->type;
  frame->meta.width = desc->width;
  frame->meta.height = desc->height;
  frame->meta.depth = desc->depth ? desc->depth : 1;
  frame->meta.format = desc->format;
  frame->meta.stride = desc->stride;
  frame->meta.modifier = desc->modifier;
  frame->meta.size = (uint32_t)size;
  return frame;
}

void texlink_frame_destroy(texlink_frame_t *frame) {
  if (!frame)
    return;
  if (frame->map_base)
    UnmapViewOfFile(frame->map_base);
  if ((frame->handle.flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED) &&
      is_win32_handle_type(frame->handle.type) && frame->handle.value.ptr)
    CloseHandle((HANDLE)frame->handle.value.ptr);
  if (frame->sync_handle)
    CloseHandle(frame->sync_handle);
  free(frame);
}

int texlink_frame_should_flip_y(texlink_backend_t producer,
                                texlink_backend_t consumer) {
  if (producer == TEXLINK_BACKEND_UNKNOWN ||
      consumer == TEXLINK_BACKEND_UNKNOWN)
    return 0;
  if (producer == consumer)
    return 0;
  if (producer == TEXLINK_BACKEND_EGL && consumer == TEXLINK_BACKEND_VULKAN)
    return 1;
  if (producer == TEXLINK_BACKEND_VULKAN && consumer == TEXLINK_BACKEND_EGL)
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
  if (!frame || !out_handle || type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return -EINVAL;
  if (type != frame->handle.type) {
    if (!(type == TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE &&
          is_shared_win32_memory_handle(frame->handle.type)))
      return -ENOTSUP;
  }
  if (frame->handle.type == TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE &&
      type != TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE)
    return -ENOTSUP;
  if (type == TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE &&
      frame->handle.type != TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE)
    return -ENOTSUP;
  if (!frame->handle.value.ptr)
    return -ENOENT;

  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->version = 1;
  out_handle->type = frame->handle.type;
  out_handle->flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  out_handle->value.ptr = frame->handle.value.ptr;
  return 0;
}

int texlink_frame_dup_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  texlink_native_handle_t borrowed;
  int ret = texlink_frame_get_native_handle(frame, type, &borrowed);
  if (ret != 0)
    return ret;

  HANDLE dup_handle = (HANDLE)borrowed.value.ptr;
  uint32_t flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  if (type != TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE) {
    if (!DuplicateHandle(GetCurrentProcess(), (HANDLE)borrowed.value.ptr,
                         GetCurrentProcess(), &dup_handle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
      return -EIO;
    flags =
        TEXLINK_NATIVE_HANDLE_FLAG_OWNED | TEXLINK_NATIVE_HANDLE_FLAG_DUPLICATED;
  }

  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->version = 1;
  out_handle->type = type;
  out_handle->flags = flags;
  out_handle->value.ptr = dup_handle;
  return 0;
}

int texlink_frame_set_sync_native_handle(texlink_frame_t *frame,
                                         const texlink_native_handle_t *handle,
                                         uint64_t value) {
  if (!frame || !handle || !is_win32_handle_type(handle->type) ||
      !handle->value.ptr)
    return -EINVAL;
  if (handle->type != TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE)
    return -ENOTSUP;

  HANDLE sync_handle = (HANDLE)handle->value.ptr;
  if (!(handle->flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED)) {
    HANDLE dup_handle = NULL;
    if (!DuplicateHandle(GetCurrentProcess(), sync_handle, GetCurrentProcess(),
                         &dup_handle, 0, FALSE, DUPLICATE_SAME_ACCESS))
      return -EIO;
    sync_handle = dup_handle;
  }

  if (frame->sync_handle)
    CloseHandle(frame->sync_handle);
  frame->sync_handle = sync_handle;
  frame->meta.sync_handle_type = (uint32_t)handle->type;
  frame->meta.sync_value = value;
  return 0;
}

int texlink_frame_get_sync_native_handle(texlink_frame_t *frame,
                                         texlink_native_handle_type_t type,
                                         texlink_native_handle_t *out_handle,
                                         uint64_t *out_value) {
  if (!frame || !out_handle || type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return -EINVAL;
  if (type != (texlink_native_handle_type_t)frame->meta.sync_handle_type)
    return -ENOTSUP;
  if (!frame->sync_handle)
    return -ENOENT;

  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->version = 1;
  out_handle->type = type;
  out_handle->flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  out_handle->value.ptr = frame->sync_handle;
  if (out_value)
    *out_value = frame->meta.sync_value;
  return 0;
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
  if (!(handle->flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED))
    return -EPERM;
  if (!is_win32_handle_type(handle->type) || !handle->value.ptr)
    return -EINVAL;

  if (!CloseHandle((HANDLE)handle->value.ptr))
    return -EIO;
  memset(handle, 0, sizeof(*handle));
  return 0;
}
