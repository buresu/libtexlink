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

/*
 * CPU cache coherency sync — required on non-coherent systems (ARM, etc.).
 * Must be called around every CPU read/write of a DMA-BUF mapped region.
 */
int texlink_buf_cpu_begin(texlink_buf_t *buf, int write) {
  if (!buf || buf->dma_fd < 0)
    return -1;

  struct dma_buf_sync sync = {
      .flags =
          DMA_BUF_SYNC_START | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
  };
  return ioctl(buf->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int texlink_buf_cpu_end(texlink_buf_t *buf, int write) {
  if (!buf || buf->dma_fd < 0)
    return -1;

  struct dma_buf_sync sync = {
      .flags =
          DMA_BUF_SYNC_END | (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ),
  };
  return ioctl(buf->dma_fd, DMA_BUF_IOCTL_SYNC, &sync);
}
