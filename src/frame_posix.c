#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <linux/dma-heap.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static texlink_frame_t *alloc_gbm(uint32_t w, uint32_t h, uint32_t format,
                                  texlink_frame_type_t type);

static int is_posix_fd_handle(texlink_native_handle_type_t type);

static void frame_set_fd_handle(texlink_frame_t *frame,
                                texlink_native_handle_type_t type, int fd,
                                uint32_t flags) {
  if (!frame)
    return;
  frame->handle.type = type;
  frame->handle.flags = flags;
  frame->handle.value.fd = fd;
  frame->meta.handle_type = (uint32_t)type;
  frame->dma_fd = (type == TEXLINK_NATIVE_HANDLE_DMA_BUF_FD) ? fd : -1;
}

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

static int open_drm_render_node(void) {
  char path[32];
  for (int i = 128; i < 140; i++) {
    snprintf(path, sizeof(path), "/dev/dri/renderD%d", i);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0)
      return fd;
  }
  return -1;
}

static int alloc_dma_heap(size_t size) {
  static const char *heaps[] = {
      "/dev/dma_heap/system",
      "/dev/dma_heap/linux,cma",
      "/dev/dma_heap/reserved",
      NULL,
  };

  for (int i = 0; heaps[i]; i++) {
    int heap_fd = open(heaps[i], O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
      continue;

    struct dma_heap_allocation_data data = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
    };

    int ret = ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data);
    close(heap_fd);
    if (ret == 0)
      return (int)data.fd;
  }
  return -1;
}

static texlink_frame_t *alloc_linear(size_t sz, uint32_t w, uint32_t h,
                                     uint32_t stride, uint32_t format,
                                     texlink_frame_type_t type) {
  int dma_fd = alloc_dma_heap(sz);

  if (dma_fd >= 0) {
    /* Fast path: dma_heap available */
    texlink_frame_t *frame = calloc(1, sizeof(*frame));
    if (!frame) {
      close(dma_fd);
      return NULL;
    }
    frame->dma_fd = dma_fd;
    frame->sync_fd = -1;
    frame->index = -1;
    frame->map_base = MAP_FAILED;
    frame->map_ptr = MAP_FAILED;
    frame->drm_fd = -1;
    frame->size = sz;
    frame->meta.type = type;
    frame_set_fd_handle(frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD, dma_fd,
                        TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
    frame->meta.width = w;
    frame->meta.height = h;
    frame->meta.depth = 1;
    frame->meta.format = format;
    frame->meta.stride = stride ? stride : infer_stride(w, format);
    frame->meta.size = sz;
    return frame;
  }

  /*
   * dma_heap not accessible (e.g. root-only). Fall back to GBM with
   * R8 (1 byte/px) shaped to cover the requested byte count.
   * The allocated stride may be larger due to GPU alignment, which is fine.
   */
  uint32_t gbm_w = (sz <= 4096) ? (uint32_t)sz : 4096u;
  uint32_t gbm_h = (uint32_t)((sz + gbm_w - 1) / gbm_w);

  texlink_frame_t *frame =
      alloc_gbm(gbm_w, gbm_h, TEXLINK_FRAME_FORMAT_R8, type);
  if (!frame)
    return NULL;

  /* Override meta to expose original logical dimensions to the caller */
  frame->meta.width = w;
  frame->meta.height = h;
  frame->meta.format = format;
  frame->meta.stride = stride ? stride : infer_stride(w, format);
  frame->meta.size = sz; /* logical size; frame->size is physical */
  return frame;
}

static texlink_frame_t *alloc_gbm(uint32_t w, uint32_t h, uint32_t format,
                                  texlink_frame_type_t type) {
  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame)
    return NULL;

  frame->dma_fd = -1;
  frame->sync_fd = -1;
  frame->index = -1;
  frame->map_base = MAP_FAILED;
  frame->map_ptr = MAP_FAILED;

  frame->drm_fd = open_drm_render_node();
  if (frame->drm_fd < 0) {
    free(frame);
    return NULL;
  }

  frame->gbm = gbm_create_device(frame->drm_fd);
  if (!frame->gbm) {
    close(frame->drm_fd);
    free(frame);
    return NULL;
  }

  uint32_t real_h = (type == TEXLINK_FRAME_TYPE_TEXTURE_CUBE) ? h * 6 : h;

  /* Try linear first for CPU-accessible export, fall back to default */
  frame->bo = gbm_bo_create(frame->gbm, w, real_h, format,
                            GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (!frame->bo)
    frame->bo =
        gbm_bo_create(frame->gbm, w, real_h, format, GBM_BO_USE_RENDERING);
  if (!frame->bo) {
    gbm_device_destroy(frame->gbm);
    close(frame->drm_fd);
    free(frame);
    return NULL;
  }

  frame->dma_fd = gbm_bo_get_fd(frame->bo);
  if (frame->dma_fd < 0) {
    gbm_bo_destroy(frame->bo);
    gbm_device_destroy(frame->gbm);
    close(frame->drm_fd);
    free(frame);
    return NULL;
  }

  uint32_t stride = gbm_bo_get_stride(frame->bo);
  frame_set_fd_handle(frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD, frame->dma_fd,
                      TEXLINK_NATIVE_HANDLE_FLAG_OWNED);
  frame->size = (size_t)stride * real_h;
  frame->meta.type = type;
  frame->meta.width = w;
  frame->meta.height = h;
  frame->meta.depth = (type == TEXLINK_FRAME_TYPE_TEXTURE_3D) ? 1 : 1;
  frame->meta.stride = stride;
  frame->meta.format = format;
  frame->meta.modifier = gbm_bo_get_modifier(frame->bo);
  frame->meta.size = frame->size;
  return frame;
}

texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc) {
  if (!desc)
    return NULL;
  if (desc->size > UINT32_MAX)
    return NULL;

  uint32_t h = desc->height ? desc->height : 1;
  uint32_t stride =
      desc->stride ? desc->stride : infer_stride(desc->width, desc->format);
  size_t size = desc->size ? (size_t)desc->size : (size_t)stride * h;
  if (size == 0)
    size = (size_t)desc->width * h;

  switch (desc->type) {
  case TEXLINK_FRAME_TYPE_RAW:
  case TEXLINK_FRAME_TYPE_VERTEX_BUFFER:
  case TEXLINK_FRAME_TYPE_COMPUTE_BUFFER:
    if (size == 0)
      return NULL;
    return alloc_linear(size, desc->width ? desc->width : (uint32_t)size, h,
                        stride, desc->format, desc->type);

  case TEXLINK_FRAME_TYPE_TEXTURE_2D:
  case TEXLINK_FRAME_TYPE_TEXTURE_3D:
  case TEXLINK_FRAME_TYPE_TEXTURE_CUBE:
    if (desc->width == 0 || desc->height == 0)
      return NULL;
    return alloc_gbm(desc->width, desc->height, desc->format, desc->type);

  default:
    return NULL;
  }
}

texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc) {
  if (!desc)
    return NULL;
  if (!is_posix_fd_handle(desc->handle.type))
    return NULL;
  if (desc->handle.value.fd < 0)
    return NULL;
  if (desc->size > UINT32_MAX)
    return NULL;

  uint64_t size = desc->size;
  if (size == 0 && desc->stride != 0 && desc->height != 0)
    size = (uint64_t)desc->stride * desc->height;
  if (size > UINT32_MAX)
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
  frame->size = (size_t)size;
  frame->meta.backend = (uint32_t)desc->backend;
  frame->meta.type = desc->type;
  frame->meta.width = desc->width;
  frame->meta.height = desc->height;
  frame->meta.depth = desc->depth ? desc->depth : 1;
  frame->meta.format = desc->format;
  frame->meta.stride = desc->stride;
  frame->meta.modifier = desc->modifier;
  frame->meta.size = size;
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
  if (frame->bo)
    gbm_bo_destroy(frame->bo);
  if (frame->gbm)
    gbm_device_destroy(frame->gbm);
  if (frame->drm_fd >= 0)
    close(frame->drm_fd);

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
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_DMA_BUF_FD:
    if (frame->handle.type == TEXLINK_NATIVE_HANDLE_DMA_BUF_FD)
      return frame->handle.value.fd;
    return frame->dma_fd;
  case TEXLINK_NATIVE_HANDLE_SYNC_FD:
    return frame->sync_fd;
  default:
    if (frame->handle.type == type && is_posix_fd_handle(type))
      return frame->handle.value.fd;
    return -1;
  }
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

int texlink_frame_get_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  if (!frame || !out_handle)
    return -EINVAL;
  if (type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return -EINVAL;
  if (type != TEXLINK_NATIVE_HANDLE_SYNC_FD && type != frame->handle.type)
    return -ENOTSUP;

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
  if (!is_posix_fd_handle(type))
    return -ENOTSUP;

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

  int ret = 0;
  if (is_posix_fd_handle(handle->type)) {
    if (handle->value.fd < 0)
      return -EINVAL;
    ret = close(handle->value.fd);
  } else {
    return -ENOTSUP;
  }

  if (ret < 0)
    return -errno;

  memset(handle, 0, sizeof(*handle));
  handle->value.fd = -1;
  return 0;
}
