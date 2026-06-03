#include "texlink_internal.h"

int texlink_export_sync_file(int dma_fd) {
  (void)dma_fd;
  return -1;
}

int texlink_wait_sync_file(int sync_fd, int timeout_ms) {
  (void)sync_fd;
  (void)timeout_ms;
  return 0;
}
