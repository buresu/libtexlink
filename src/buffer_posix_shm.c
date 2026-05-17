#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

static int is_posix_fd_handle(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_DMA_BUF_FD:
  case TEXLINK_NATIVE_HANDLE_SYNC_FD:
  case TEXLINK_NATIVE_HANDLE_OPAQUE_FD:
    return 1;
  default:
    return 0;
  }
}

static void frame_set_fd_handle(texlink_frame_t *frame,
                                texlink_native_handle_type_t type, int fd,
                                uint32_t flags) {
  frame->handle.type = type;
  frame->handle.flags = flags;
  frame->handle.value.fd = fd;
  frame->meta.handle_type = (uint32_t)type;
  frame->dma_fd = fd;
}

static int alloc_shared_fd(size_t size) {
  char name[64];
  snprintf(name, sizeof(name), TEXLINK_SHM_PREFIX "cpu_%ld_%p", (long)getpid(),
           (void *)&size);

  int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  shm_unlink(name);

  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static texlink_frame_t *alloc_linear(size_t size, uint32_t w, uint32_t h,
                                     uint32_t stride, uint32_t format,
                                     texlink_frame_type_t type) {
  int fd = alloc_shared_fd(size);
  if (fd < 0)
    return NULL;

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    close(fd);
    return NULL;
  }

  frame_set_fd_handle(frame, TEXLINK_NATIVE_HANDLE_OPAQUE_FD, fd,
                      TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
  frame->sync_fd = -1;
  frame->index = -1;
  frame->map_base = MAP_FAILED;
  frame->map_ptr = MAP_FAILED;
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
    return alloc_linear(size, desc->width ? desc->width : (uint32_t)size, h,
                        stride, desc->format, desc->type);
  default:
    return NULL;
  }
}

texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc) {
  if (!desc || !is_posix_fd_handle(desc->handle.type) ||
      desc->handle.value.fd < 0 || desc->size > UINT32_MAX)
    return NULL;

  int fd = desc->handle.value.fd;
  if (!(desc->handle.flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED)) {
    fd = dup(desc->handle.value.fd);
    if (fd < 0)
      return NULL;
  }

  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame) {
    close(fd);
    return NULL;
  }

  frame_set_fd_handle(frame, desc->handle.type, fd,
                      TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
  frame->sync_fd = -1;
  frame->index = -1;
  frame->map_base = MAP_FAILED;
  frame->map_ptr = MAP_FAILED;
  frame->drm_fd = -1;
  frame->size = (size_t)desc->size;
  frame->meta.backend = (uint32_t)desc->backend;
  frame->meta.type = desc->type;
  frame->meta.width = desc->width;
  frame->meta.height = desc->height;
  frame->meta.depth = desc->depth ? desc->depth : 1;
  frame->meta.format = desc->format;
  frame->meta.stride = desc->stride;
  frame->meta.modifier = desc->modifier;
  frame->meta.size = (uint32_t)desc->size;
  return frame;
}

void texlink_frame_destroy(texlink_frame_t *frame) {
  if (!frame)
    return;
  if (frame->map_base != MAP_FAILED && frame->map_base)
    munmap(frame->map_base, frame->map_size);
  if (frame->sync_fd >= 0)
    close(frame->sync_fd);
  if ((frame->handle.flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED) &&
      is_posix_fd_handle(frame->handle.type) && frame->handle.value.fd >= 0)
    close(frame->handle.value.fd);
  else if (frame->dma_fd >= 0)
    close(frame->dma_fd);
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

static int frame_fd_for_handle(texlink_frame_t *frame,
                               texlink_native_handle_type_t type) {
  if (type == TEXLINK_NATIVE_HANDLE_SYNC_FD)
    return frame->sync_fd;
  if (frame->handle.type == type && is_posix_fd_handle(type))
    return frame->handle.value.fd;
  return -1;
}

int texlink_frame_get_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  if (!frame || !out_handle || type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return -EINVAL;

  int fd = frame_fd_for_handle(frame, type);
  if (fd < 0)
    return -ENOENT;

  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->type = type;
  out_handle->flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  out_handle->value.fd = fd;
  return 0;
}

int texlink_frame_dup_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  texlink_native_handle_t borrowed;
  int ret = texlink_frame_get_native_handle(frame, type, &borrowed);
  if (ret != 0)
    return ret;

  int fd = dup(borrowed.value.fd);
  if (fd < 0)
    return -errno;

  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->type = type;
  out_handle->flags =
      TEXLINK_NATIVE_HANDLE_FLAG_OWNED | TEXLINK_NATIVE_HANDLE_FLAG_DUPLICATED;
  out_handle->value.fd = fd;
  return 0;
}

int texlink_frame_set_sync_native_handle(texlink_frame_t *frame,
                                         const texlink_native_handle_t *handle,
                                         uint64_t value) {
  if (!frame || !handle || handle->type != TEXLINK_NATIVE_HANDLE_SYNC_FD)
    return -EINVAL;
  int fd = handle->value.fd;
  if (!(handle->flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED)) {
    fd = dup(handle->value.fd);
    if (fd < 0)
      return -errno;
  }
  if (frame->sync_fd >= 0)
    close(frame->sync_fd);
  frame->sync_fd = fd;
  frame->meta.sync_handle_type = (uint32_t)handle->type;
  frame->meta.sync_value = value;
  return 0;
}

int texlink_frame_get_sync_native_handle(texlink_frame_t *frame,
                                         texlink_native_handle_type_t type,
                                         texlink_native_handle_t *out_handle,
                                         uint64_t *out_value) {
  if (!frame || !out_handle || type != TEXLINK_NATIVE_HANDLE_SYNC_FD)
    return -EINVAL;
  if (frame->sync_fd < 0)
    return -ENOENT;
  memset(out_handle, 0, sizeof(*out_handle));
  out_handle->type = type;
  out_handle->flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  out_handle->value.fd = frame->sync_fd;
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
  if (!is_posix_fd_handle(handle->type) || handle->value.fd < 0)
    return -EINVAL;

  int ret = close(handle->value.fd);
  if (ret < 0)
    return -errno;

  memset(handle, 0, sizeof(*handle));
  handle->value.fd = -1;
  return 0;
}
