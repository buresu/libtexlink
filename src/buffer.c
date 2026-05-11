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

static texlink_buf_t *alloc_gbm(uint32_t w, uint32_t h, uint32_t format,
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

static texlink_buf_t *alloc_linear(uint32_t w, uint32_t h, uint32_t format,
                                   texlink_type_t type) {
  size_t sz = (size_t)w * (h ? h : 1);

  int dma_fd = alloc_dma_heap(sz);

  if (dma_fd >= 0) {
    /* Fast path: dma_heap available */
    texlink_buf_t *buf = calloc(1, sizeof(*buf));
    if (!buf) {
      close(dma_fd);
      return NULL;
    }
    buf->dma_fd = dma_fd;
    buf->sync_fd = -1;
    buf->map_ptr = MAP_FAILED;
    buf->drm_fd = -1;
    buf->size = sz;
    buf->meta.type = type;
    buf->meta.width = w;
    buf->meta.height = h;
    buf->meta.depth = 1;
    buf->meta.format = format;
    buf->meta.stride = w;
    buf->meta.size = (uint32_t)sz;
    return buf;
  }

  /*
   * dma_heap not accessible (e.g. root-only). Fall back to GBM with
   * DRM_FORMAT_R8 (1 byte/px) shaped to cover the requested byte count.
   * The allocated stride may be larger due to GPU alignment, which is fine.
   */
  uint32_t gbm_w = (sz <= 4096) ? (uint32_t)sz : 4096u;
  uint32_t gbm_h = (uint32_t)((sz + gbm_w - 1) / gbm_w);

  texlink_buf_t *buf = alloc_gbm(gbm_w, gbm_h, DRM_FORMAT_R8, type);
  if (!buf)
    return NULL;

  /* Override meta to expose original logical dimensions to the caller */
  buf->meta.width = w;
  buf->meta.height = h;
  buf->meta.format = format;
  buf->meta.size = (uint32_t)sz; /* logical size; buf->size is physical */
  return buf;
}

static texlink_buf_t *alloc_gbm(uint32_t w, uint32_t h, uint32_t format,
                                texlink_type_t type) {
  texlink_buf_t *buf = calloc(1, sizeof(*buf));
  if (!buf)
    return NULL;

  buf->dma_fd = -1;
  buf->sync_fd = -1;
  buf->map_ptr = MAP_FAILED;

  buf->drm_fd = open_drm_render_node();
  if (buf->drm_fd < 0) {
    free(buf);
    return NULL;
  }

  buf->gbm = gbm_create_device(buf->drm_fd);
  if (!buf->gbm) {
    close(buf->drm_fd);
    free(buf);
    return NULL;
  }

  uint32_t real_h = (type == TEXLINK_TYPE_TEXTURE_CUBE) ? h * 6 : h;

  /* Try linear first for CPU-accessible export, fall back to default */
  buf->bo = gbm_bo_create(buf->gbm, w, real_h, format,
                          GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  if (!buf->bo)
    buf->bo = gbm_bo_create(buf->gbm, w, real_h, format, GBM_BO_USE_RENDERING);
  if (!buf->bo) {
    gbm_device_destroy(buf->gbm);
    close(buf->drm_fd);
    free(buf);
    return NULL;
  }

  buf->dma_fd = gbm_bo_get_fd(buf->bo);
  if (buf->dma_fd < 0) {
    gbm_bo_destroy(buf->bo);
    gbm_device_destroy(buf->gbm);
    close(buf->drm_fd);
    free(buf);
    return NULL;
  }

  uint32_t stride = gbm_bo_get_stride(buf->bo);
  buf->size = (size_t)stride * real_h;
  buf->meta.type = type;
  buf->meta.width = w;
  buf->meta.height = h;
  buf->meta.depth = (type == TEXLINK_TYPE_TEXTURE_3D) ? 1 : 1;
  buf->meta.stride = stride;
  buf->meta.format = format;
  buf->meta.modifier = gbm_bo_get_modifier(buf->bo);
  buf->meta.size = (uint32_t)buf->size;
  return buf;
}

texlink_buf_t *texlink_buf_alloc(uint32_t w, uint32_t h, uint32_t format,
                                 texlink_type_t type) {
  switch (type) {
  case TEXLINK_TYPE_RAW:
  case TEXLINK_TYPE_VERTEX_BUFFER:
  case TEXLINK_TYPE_COMPUTE_BUFFER:
    return alloc_linear(w, h, format, type);

  case TEXLINK_TYPE_TEXTURE_2D:
  case TEXLINK_TYPE_TEXTURE_3D:
  case TEXLINK_TYPE_TEXTURE_CUBE:
    return alloc_gbm(w, h, format, type);

  default:
    return NULL;
  }
}

void texlink_buf_free(texlink_buf_t *buf) {
  if (!buf)
    return;

  if (buf->map_ptr != MAP_FAILED && buf->map_ptr)
    munmap(buf->map_ptr, buf->size);
  if (buf->sync_fd >= 0)
    close(buf->sync_fd);
  if (buf->dma_fd >= 0)
    close(buf->dma_fd);
  if (buf->bo)
    gbm_bo_destroy(buf->bo);
  if (buf->gbm)
    gbm_device_destroy(buf->gbm);
  if (buf->drm_fd >= 0)
    close(buf->drm_fd);

  free(buf);
}

texlink_meta_t texlink_buf_meta(texlink_buf_t *buf) {
  if (!buf) {
    texlink_meta_t zero = {0};
    return zero;
  }
  return buf->meta;
}

int texlink_buf_get_dma_fd(texlink_buf_t *buf) {
  return buf ? buf->dma_fd : -1;
}

int texlink_buf_get_sync_fd(texlink_buf_t *buf) {
  return buf ? buf->sync_fd : -1;
}

void *texlink_buf_map(texlink_buf_t *buf) {
  if (!buf)
    return NULL;
  if (buf->map_ptr != MAP_FAILED && buf->map_ptr)
    return buf->map_ptr;

  buf->map_ptr =
      mmap(NULL, buf->size, PROT_READ | PROT_WRITE, MAP_SHARED, buf->dma_fd, 0);
  if (buf->map_ptr == MAP_FAILED) {
    buf->map_ptr = NULL;
    return NULL;
  }
  return buf->map_ptr;
}

void texlink_buf_unmap(texlink_buf_t *buf) {
  if (!buf || !buf->map_ptr || buf->map_ptr == MAP_FAILED)
    return;

  munmap(buf->map_ptr, buf->size);
  buf->map_ptr = NULL;
}
