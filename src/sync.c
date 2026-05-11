#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>

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

static uint64_t dma_buf_sync_flags(uint32_t access) {
  if ((access & TEXLINK_ACCESS_READ) && (access & TEXLINK_ACCESS_WRITE))
    return DMA_BUF_SYNC_RW;
  if (access & TEXLINK_ACCESS_WRITE)
    return DMA_BUF_SYNC_WRITE;
  return DMA_BUF_SYNC_READ;
}

void *texlink_frame_begin_access(texlink_frame_t *frame, uint32_t access) {
  if (!frame || frame->dma_fd < 0)
    return NULL;
  if (!(access & (TEXLINK_ACCESS_READ | TEXLINK_ACCESS_WRITE)))
    return NULL;
  if (frame->active_access)
    return NULL;

  void *ptr = texlink_frame_map(frame);
  if (!ptr)
    return NULL;

  struct dma_buf_sync sync = {
      .flags = DMA_BUF_SYNC_START | dma_buf_sync_flags(access),
  };
  if (ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
    return NULL;

  frame->active_access = access;
  return ptr;
}

int texlink_frame_end_access(texlink_frame_t *frame) {
  if (!frame || frame->dma_fd < 0)
    return -1;
  if (!frame->active_access)
    return -EINVAL;

  struct dma_buf_sync sync = {
      .flags = DMA_BUF_SYNC_END | dma_buf_sync_flags(frame->active_access),
  };
  frame->active_access = 0;
  return ioctl(frame->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
}
