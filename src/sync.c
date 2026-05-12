#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int texlink_export_sync_file(int dma_fd) {
  struct dma_buf_export_sync_file req = {
      .flags = DMA_BUF_SYNC_RW,
      .fd = -1,
  };
  if (ioctl(dma_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &req) < 0)
    return -1;
  return req.fd;
}

int texlink_wait_sync_file(int sync_fd, int timeout_ms) {
  if (sync_fd < 0)
    return 0;

  struct pollfd pfd = {.fd = sync_fd, .events = POLLIN};
  int ret = poll(&pfd, 1, timeout_ms);
  if (ret < 0)
    return -1;
  if (ret == 0)
    return -ETIMEDOUT;
  return 0;
}

static int normalize_range(uint64_t frame_size, uint64_t offset,
                           uint64_t requested_size, uint64_t *out_size) {
  if (offset > frame_size)
    return -EINVAL;

  uint64_t size = requested_size;
  if (size == 0)
    size = frame_size - offset;
  if (offset + size < offset || offset + size > frame_size)
    return -EINVAL;

  *out_size = size;
  return 0;
}

static uint64_t dma_buf_sync_flags(uint32_t access) {
  if ((access & TEXLINK_CPU_ACCESS_READ) && (access & TEXLINK_CPU_ACCESS_WRITE))
    return DMA_BUF_SYNC_RW;
  if (access & TEXLINK_CPU_ACCESS_WRITE)
    return DMA_BUF_SYNC_WRITE;
  return DMA_BUF_SYNC_READ;
}

static int map_prot(uint32_t flags) {
  int prot = 0;
  if (flags & TEXLINK_MAP_READ)
    prot |= PROT_READ;
  if (flags & TEXLINK_MAP_WRITE)
    prot |= PROT_WRITE;
  return prot;
}

int texlink_frame_map(texlink_frame_t *frame, const texlink_map_desc_t *desc,
                      texlink_mapping_t *out_mapping) {
  if (!frame || frame->dma_fd < 0 || !desc || !out_mapping)
    return -EINVAL;
  if (!(desc->flags & (TEXLINK_MAP_READ | TEXLINK_MAP_WRITE)))
    return -EINVAL;

  if (frame->map_ptr != MAP_FAILED && frame->map_ptr) {
    out_mapping->data = frame->map_ptr;
    out_mapping->size = frame->map_length;
    out_mapping->stride = frame->meta.stride;
    return 0;
  }

  uint64_t map_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &map_size);
  if (ret != 0)
    return ret;
  if (map_size == 0)
    return -EINVAL;

  long page_size = sysconf(_SC_PAGE_SIZE);
  if (page_size <= 0)
    return -EINVAL;

  uint64_t page_mask = (uint64_t)page_size - 1u;
  uint64_t mmap_offset = desc->offset & ~page_mask;
  uint64_t delta = desc->offset - mmap_offset;
  uint64_t mmap_size = map_size + delta;

  void *base = mmap(NULL, (size_t)mmap_size, map_prot(desc->flags), MAP_SHARED,
                    frame->dma_fd, (off_t)mmap_offset);
  if (base == MAP_FAILED)
    return -errno;

  frame->map_base = base;
  frame->map_ptr = (uint8_t *)base + delta;
  frame->map_size = (size_t)mmap_size;
  frame->map_length = map_size;
  frame->map_offset = desc->offset;
  frame->map_flags = desc->flags;

  out_mapping->data = frame->map_ptr;
  out_mapping->size = map_size;
  out_mapping->stride = frame->meta.stride;
  return 0;
}

int texlink_frame_unmap(texlink_frame_t *frame) {
  if (!frame || frame->dma_fd < 0)
    return -EINVAL;
  if (frame->active_access)
    return -EBUSY;
  if (frame->map_base == MAP_FAILED || !frame->map_base)
    return 0;

  int ret = munmap(frame->map_base, frame->map_size);
  frame->map_base = MAP_FAILED;
  frame->map_ptr = MAP_FAILED;
  frame->map_size = 0;
  frame->map_length = 0;
  frame->map_offset = 0;
  frame->map_flags = 0;
  return ret == 0 ? 0 : -errno;
}

int texlink_frame_is_mapped(texlink_frame_t *frame) {
  return frame && frame->map_ptr != MAP_FAILED && frame->map_ptr;
}

int texlink_frame_cpu_begin(texlink_frame_t *frame,
                            const texlink_cpu_access_desc_t *desc) {
  if (!frame || frame->dma_fd < 0 || !desc)
    return -EINVAL;
  if (!texlink_frame_is_mapped(frame))
    return -EINVAL;
  if (!(desc->access & (TEXLINK_CPU_ACCESS_READ | TEXLINK_CPU_ACCESS_WRITE)))
    return -EINVAL;
  if (frame->active_access)
    return -EBUSY;

  uint64_t access_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &access_size);
  if (ret != 0)
    return ret;

  struct dma_buf_sync sync = {
      .flags = DMA_BUF_SYNC_START | dma_buf_sync_flags(desc->access),
  };
  if (ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    return -errno;

  frame->active_access = desc->access;
  frame->active_access_offset = desc->offset;
  frame->active_access_size = access_size;
  return 0;
}

int texlink_frame_cpu_end(texlink_frame_t *frame,
                          const texlink_cpu_access_desc_t *desc) {
  if (!frame || frame->dma_fd < 0 || !desc)
    return -EINVAL;
  if (!texlink_frame_is_mapped(frame))
    return -EINVAL;
  if (!frame->active_access)
    return -EINVAL;
  if (desc->access != frame->active_access)
    return -EINVAL;

  uint64_t access_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &access_size);
  if (ret != 0)
    return ret;
  if (desc->offset != frame->active_access_offset ||
      access_size != frame->active_access_size)
    return -EINVAL;

  struct dma_buf_sync sync = {
      .flags = DMA_BUF_SYNC_END | dma_buf_sync_flags(frame->active_access),
  };
  ret = ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
  frame->active_access = 0;
  frame->active_access_offset = 0;
  frame->active_access_size = 0;
  return ret == 0 ? 0 : -errno;
}
