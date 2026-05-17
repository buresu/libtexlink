#include "texlink_internal.h"

#include <stdio.h>
#include <string.h>

texlink_os_handle_t texlink_shm_create(const char *name, size_t size) {
  return CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                            (DWORD)size, name);
}

texlink_os_handle_t texlink_shm_open(const char *name) {
  return OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
}

void *texlink_shm_map(texlink_os_handle_t handle, size_t size) {
  return MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
}

int texlink_shm_map_failed(void *ptr) {
  return ptr == NULL;
}

void texlink_shm_unmap(void *ptr, size_t size) {
  (void)size;
  UnmapViewOfFile(ptr);
}

void texlink_shm_close(texlink_os_handle_t handle) {
  if (handle)
    CloseHandle(handle);
}

void texlink_shm_unlink(const char *name) {
  (void)name;
}

void texlink_make_shm_name(const char *path, char *out, size_t len) {
  const char *base = strrchr(path, '/');
  const char *backslash = strrchr(path, '\\');
  if (backslash && (!base || backslash > base))
    base = backslash;
  base = base ? base + 1 : path;
  snprintf(out, len, TEXLINK_SHM_PREFIX "%s", base);

  char *dot = strrchr(out, '.');
  if (dot)
    *dot = '\0';
}

void texlink_make_socket_dir(const char *path) {
  (void)path;
}

void texlink_cleanup_socket_path(const char *path) {
  (void)path;
}

void texlink_default_path_for_name(const char *name, char *path, size_t len) {
  snprintf(path, len, "\\\\.\\pipe\\texlink_%s", name);
}

int texlink_native_handle_type_is_ipc(texlink_native_handle_type_t type) {
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

texlink_native_handle_type_t texlink_default_native_handle_type(void) {
  return TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE;
}

void texlink_frame_init_received(texlink_frame_t *frame) {
  frame->dma_fd = -1;
  frame->sync_fd = -1;
  frame->sync_handle = NULL;
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->drm_fd = -1;
}

int texlink_frame_recv_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type) {
  HANDLE handle = NULL;
  if (type == TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE) {
    uintptr_t wire = 0;
    if (texlink_socket_recv(sock, &wire, sizeof(wire)) != 0)
      return -1;
    handle = (HANDLE)wire;
  } else {
    if (texlink_recv_ipc_handles(sock, &handle, 1) < 0)
      return -1;
  }

  frame->handle.handle_type = type;
  frame->handle.owned = (type == TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE) ? 0 : 1;
  frame->handle.value.ptr = handle;
  frame->win32_handle = handle;
  frame->meta.handle_type = (uint32_t)type;
  frame->dma_fd = -1;
  return 0;
}

int texlink_frame_recv_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type) {
  if (!frame || type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return 0;
  HANDLE handle = NULL;
  if (texlink_recv_ipc_handles(sock, &handle, 1) < 0)
    return -1;
  frame->sync_handle = handle;
  frame->meta.sync_handle_type = (uint32_t)type;
  return 0;
}

int texlink_frame_send_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type) {
  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(frame, type, &handle) != 0 ||
      !texlink_native_handle_type_is_ipc(handle.handle_type))
    return -1;
  if (type == TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE) {
    uintptr_t wire = (uintptr_t)handle.value.ptr;
    return texlink_socket_send(sock, &wire, sizeof(wire));
  }
  texlink_ipc_handle_t ipc_handle = (HANDLE)handle.value.ptr;
  return texlink_send_ipc_handles(sock, &ipc_handle, 1);
}

int texlink_frame_send_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type) {
  texlink_native_handle_t handle;
  uint64_t value;
  if (type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return 0;
  if (texlink_frame_get_sync_native_handle(frame, type, &handle, &value) != 0 ||
      !texlink_native_handle_type_is_ipc(handle.handle_type))
    return -1;
  texlink_ipc_handle_t ipc_handle = (HANDLE)handle.value.ptr;
  return texlink_send_ipc_handles(sock, &ipc_handle, 1);
}

void texlink_frame_close_ipc_handle(texlink_frame_t *frame) {
  if (!frame)
    return;
  if (frame->handle.owned &&
      texlink_native_handle_type_is_ipc(frame->handle.handle_type) &&
      frame->handle.value.ptr) {
    CloseHandle((HANDLE)frame->handle.value.ptr);
    frame->handle.value.ptr = NULL;
    frame->win32_handle = NULL;
  }
  frame->dma_fd = -1;
}

void texlink_frame_close_sync_handle(texlink_frame_t *frame) {
  if (!frame || !frame->sync_handle)
    return;
  CloseHandle(frame->sync_handle);
  frame->sync_handle = NULL;
}

void texlink_frame_unmap_ipc_view(texlink_frame_t *frame) {
  if (frame && frame->map_base)
    UnmapViewOfFile(frame->map_base);
}

void texlink_close_sync_fd(int sync_fd) {
  (void)sync_fd;
}
