#define _GNU_SOURCE
#include "texlink_internal.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

texlink_os_handle_t texlink_shm_create(const char *name, size_t size) {
  int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)size) < 0)
    return TEXLINK_INVALID_OS_HANDLE;
  return fd;
}

texlink_os_handle_t texlink_shm_open(const char *name) {
  return shm_open(name, O_RDWR, 0);
}

void *texlink_shm_map(texlink_os_handle_t handle, size_t size) {
  return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, handle, 0);
}

int texlink_shm_map_failed(void *ptr) {
  return ptr == MAP_FAILED;
}

void texlink_shm_unmap(void *ptr, size_t size) {
  munmap(ptr, size);
}

void texlink_shm_close(texlink_os_handle_t handle) {
  if (handle >= 0)
    close(handle);
}

void texlink_shm_unlink(const char *name) {
  shm_unlink(name);
}

void texlink_make_shm_name(const char *path, char *out, size_t len) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  snprintf(out, len, TEXLINK_SHM_PREFIX "%s", base);

  char *dot = strrchr(out, '.');
  if (dot)
    *dot = '\0';
}

void texlink_make_socket_dir(const char *path) {
  char dir[256];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    mkdir(dir, 0755);
  }
}

void texlink_cleanup_socket_path(const char *path) {
  if (path && path[0] != '\0')
    unlink(path);
}

void texlink_default_path_for_name(const char *name, char *path, size_t len) {
  snprintf(path, len, "/tmp/texlink/%s.sock", name);
}

int texlink_native_handle_type_is_ipc(texlink_native_handle_type_t type) {
  switch (type) {
  case TEXLINK_NATIVE_HANDLE_DMA_BUF_FD:
  case TEXLINK_NATIVE_HANDLE_SYNC_FD:
  case TEXLINK_NATIVE_HANDLE_OPAQUE_FD:
    return 1;
  default:
    return 0;
  }
}

texlink_native_handle_type_t texlink_default_native_handle_type(void) {
  return TEXLINK_NATIVE_HANDLE_DMA_BUF_FD;
}

void texlink_frame_init_received(texlink_frame_t *frame) {
  frame->dma_fd = -1;
  frame->sync_fd = -1;
  frame->map_base = MAP_FAILED;
  frame->map_ptr = MAP_FAILED;
  frame->drm_fd = -1;
}

int texlink_frame_recv_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type) {
  int fd = -1;
  if (texlink_recv_ipc_handles(sock, &fd, 1) < 0)
    return -1;

  frame->handle.handle_type = type;
  frame->handle.flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED;
  frame->handle.value.fd = fd;
  frame->meta.handle_type = (uint32_t)type;
  frame->dma_fd = (type == TEXLINK_NATIVE_HANDLE_DMA_BUF_FD) ? fd : -1;
  return 0;
}

int texlink_frame_recv_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type) {
  (void)sock;
  (void)frame;
  (void)type;
  return 0;
}

int texlink_frame_send_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type) {
  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(frame, type, &handle) != 0 ||
      !texlink_native_handle_type_is_ipc(handle.handle_type))
    return -1;
  return texlink_send_ipc_handles(sock, &handle.value.fd, 1);
}

int texlink_frame_send_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type) {
  (void)sock;
  (void)frame;
  (void)type;
  return 0;
}

void texlink_frame_close_ipc_handle(texlink_frame_t *frame) {
  if (!frame)
    return;
  if ((frame->handle.flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED) &&
      texlink_native_handle_type_is_ipc(frame->handle.handle_type) &&
      frame->handle.value.fd >= 0) {
    close(frame->handle.value.fd);
    frame->handle.value.fd = -1;
  } else if (frame->dma_fd >= 0) {
    close(frame->dma_fd);
  }
  frame->dma_fd = -1;
}

void texlink_frame_close_sync_handle(texlink_frame_t *frame) {
  (void)frame;
}

void texlink_frame_unmap_ipc_view(texlink_frame_t *frame) {
  if (frame && frame->map_base && frame->map_base != MAP_FAILED)
    munmap(frame->map_base, frame->map_size);
}

void texlink_close_sync_fd(int sync_fd) {
  if (sync_fd >= 0)
    close(sync_fd);
}
