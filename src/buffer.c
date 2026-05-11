#define _GNU_SOURCE
#include "texlink_internal.h"

#include <drm_fourcc.h>
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
                                  texlink_type_t type);

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
                                     uint32_t format, texlink_type_t type) {
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
    frame->map_ptr = MAP_FAILED;
    frame->drm_fd = -1;
    frame->size = sz;
    frame->meta.type = type;
    frame->meta.width = w;
    frame->meta.height = h;
    frame->meta.depth = 1;
    frame->meta.format = format;
    frame->meta.stride = w;
    frame->meta.size = (uint32_t)sz;
    return frame;
  }

  /*
   * dma_heap not accessible (e.g. root-only). Fall back to GBM with
   * DRM_FORMAT_R8 (1 byte/px) shaped to cover the requested byte count.
   * The allocated stride may be larger due to GPU alignment, which is fine.
   */
  uint32_t gbm_w = (sz <= 4096) ? (uint32_t)sz : 4096u;
  uint32_t gbm_h = (uint32_t)((sz + gbm_w - 1) / gbm_w);

  texlink_frame_t *frame = alloc_gbm(gbm_w, gbm_h, DRM_FORMAT_R8, type);
  if (!frame)
    return NULL;

  /* Override meta to expose original logical dimensions to the caller */
  frame->meta.width = w;
  frame->meta.height = h;
  frame->meta.format = format;
  frame->meta.size = (uint32_t)sz; /* logical size; frame->size is physical */
  return frame;
}

static texlink_frame_t *alloc_gbm(uint32_t w, uint32_t h, uint32_t format,
                                  texlink_type_t type) {
  texlink_frame_t *frame = calloc(1, sizeof(*frame));
  if (!frame)
    return NULL;

  frame->dma_fd = -1;
  frame->sync_fd = -1;
  frame->index = -1;
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

  uint32_t real_h = (type == TEXLINK_TYPE_TEXTURE_CUBE) ? h * 6 : h;

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
  frame->size = (size_t)stride * real_h;
  frame->meta.type = type;
  frame->meta.width = w;
  frame->meta.height = h;
  frame->meta.depth = (type == TEXLINK_TYPE_TEXTURE_3D) ? 1 : 1;
  frame->meta.stride = stride;
  frame->meta.format = format;
  frame->meta.modifier = gbm_bo_get_modifier(frame->bo);
  frame->meta.size = (uint32_t)frame->size;
  return frame;
}

texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc) {
  if (!desc)
    return NULL;
  if (desc->size > UINT32_MAX)
    return NULL;

  uint32_t h = desc->height ? desc->height : 1;
  size_t size = desc->size ? (size_t)desc->size : (size_t)desc->width * h;

  switch (desc->type) {
  case TEXLINK_TYPE_RAW:
  case TEXLINK_TYPE_VERTEX_BUFFER:
  case TEXLINK_TYPE_COMPUTE_BUFFER:
    if (size == 0)
      return NULL;
    return alloc_linear(size, desc->width ? desc->width : (uint32_t)size, h,
                        desc->format, desc->type);

  case TEXLINK_TYPE_TEXTURE_2D:
  case TEXLINK_TYPE_TEXTURE_3D:
  case TEXLINK_TYPE_TEXTURE_CUBE:
    if (desc->width == 0 || desc->height == 0)
      return NULL;
    return alloc_gbm(desc->width, desc->height, desc->format, desc->type);

  default:
    return NULL;
  }
}

void texlink_frame_destroy(texlink_frame_t *frame) {
  if (!frame)
    return;

  if (frame->map_ptr != MAP_FAILED && frame->map_ptr)
    munmap(frame->map_ptr, frame->size);
  if (frame->sync_fd >= 0)
    close(frame->sync_fd);
  if (frame->dma_fd >= 0)
    close(frame->dma_fd);
  if (frame->bo)
    gbm_bo_destroy(frame->bo);
  if (frame->gbm)
    gbm_device_destroy(frame->gbm);
  if (frame->drm_fd >= 0)
    close(frame->drm_fd);

  free(frame);
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

int texlink_frame_get_dma_fd(texlink_frame_t *frame) {
  return frame ? frame->dma_fd : -1;
}

int texlink_frame_get_sync_fd(texlink_frame_t *frame) {
  return frame ? frame->sync_fd : -1;
}

void *texlink_frame_map(texlink_frame_t *frame) {
  if (!frame)
    return NULL;
  if (frame->map_ptr != MAP_FAILED && frame->map_ptr)
    return frame->map_ptr;

  frame->map_ptr = mmap(NULL, frame->size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        frame->dma_fd, 0);
  if (frame->map_ptr == MAP_FAILED) {
    frame->map_ptr = NULL;
    return NULL;
  }
  return frame->map_ptr;
}
