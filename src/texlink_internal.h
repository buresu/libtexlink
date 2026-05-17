#pragma once

#include "texlink.h"

#include <stdatomic.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef struct texlink_win32_socket *texlink_socket_t;
typedef HANDLE texlink_os_handle_t;
typedef HANDLE texlink_ipc_handle_t;
typedef DWORD texlink_pid_t;
#define TEXLINK_INVALID_SOCKET_HANDLE NULL
#define TEXLINK_INVALID_OS_HANDLE NULL
#define TEXLINK_SOCKET_PATH_MAX 260
#define TEXLINK_REGISTRY_PATH "Local\\texlink_registry"
#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif
#else
#include <sys/types.h>
typedef int texlink_socket_t;
typedef int texlink_os_handle_t;
typedef int texlink_ipc_handle_t;
typedef pid_t texlink_pid_t;
#define TEXLINK_INVALID_SOCKET_HANDLE -1
#define TEXLINK_INVALID_OS_HANDLE -1
#define TEXLINK_SOCKET_PATH_MAX 108
#define TEXLINK_REGISTRY_PATH "/tmp/texlink/.registry"
#endif

/* GBM forward declarations to avoid hard include in this header */
struct gbm_device;
struct gbm_bo;

#define TEXLINK_MAX_BUFS 3
#ifdef _WIN32
#define TEXLINK_SHM_PREFIX "Local\\texlink_"
#else
#define TEXLINK_SHM_PREFIX "/texlink_"
#endif
#define TEXLINK_PROTO_VER 2
#define TEXLINK_NAME_MAX 64
#define TEXLINK_MAX_CLIENTS 16
#define TEXLINK_MAX_SESSIONS 64
#define TEXLINK_REGISTRY_MAGIC 0x544C4E4Bu /* TLNK */

typedef enum {
  TEXLINK_BUFFERING_SINGLE = 1,
  TEXLINK_BUFFERING_DOUBLE = 2,
  TEXLINK_BUFFERING_TRIPLE = 3,
} texlink_buffering_t;

typedef struct texlink_session texlink_session_t;

struct texlink_frame {
  texlink_native_handle_t handle;
  int dma_fd;
  int sync_fd;
#ifdef _WIN32
  HANDLE win32_handle;
  HANDLE sync_handle;
#endif
  int index;
  void *map_base;
  void *map_ptr;
  size_t map_size;
  uint64_t map_length;
  uint64_t map_offset;
  uint32_t map_flags;
  size_t size;
  uint32_t active_access;
  uint64_t active_access_offset;
  uint64_t active_access_size;
  texlink_meta_t meta;

  /* GBM-backed (texture types); NULL for dma_heap or consumer-side frames */
  struct gbm_bo *bo;
  struct gbm_device *gbm;
  int drm_fd;
};

/* Shared memory layout must be identical in producer and consumer */
typedef struct {
  _Atomic uint64_t frame_id;
  _Atomic uint32_t current_idx;
  uint32_t buf_count;
  texlink_meta_t meta;
  uint8_t _pad[40]; /* pad to 128 bytes */
} texlink_shm_t;

/* Handshake sent over socket at connect time */
typedef struct {
  uint32_t version;
  uint32_t buf_count;
  texlink_buffering_t buffering;
  uint32_t _pad;
  texlink_meta_t meta;
  char shm_name[64];
} texlink_handshake_t;

/* Per-frame notification sent by producer to consumer */
typedef struct {
  uint64_t frame_id;
  uint32_t buf_idx;
  uint32_t has_sync_fd; /* 1 if SCM_RIGHTS carries sync_fd */
  uint64_t sync_value;
} texlink_frame_msg_t;

/* Registry entry stored in TEXLINK_REGISTRY_PATH */
typedef struct {
  char name[TEXLINK_NAME_MAX];
  char path[TEXLINK_SOCKET_PATH_MAX];
  texlink_pid_t pid;
  uint32_t active;
} texlink_registry_entry_t;

typedef struct {
  uint32_t magic;
  uint32_t _pad;
  texlink_registry_entry_t entries[TEXLINK_MAX_SESSIONS];
} texlink_registry_t;

struct texlink_session {
  texlink_socket_t sock_fd;
  texlink_os_handle_t shm_fd;
  texlink_shm_t *shm;
  texlink_frame_t *frames[TEXLINK_MAX_BUFS];
  int buf_count;
  texlink_buffering_t buffering;
  int is_producer;
  int write_idx; /* producer rotation cursor */
  int is_registered;
  uint64_t last_frame_id; /* updated by consumer_acquire from socket msg */
  char shm_name[64];
  char sock_path[TEXLINK_SOCKET_PATH_MAX];
  char reg_name[TEXLINK_NAME_MAX];
};

struct texlink_server {
  texlink_socket_t listen_fd;
  texlink_socket_t client_fds[TEXLINK_MAX_CLIENTS];
  texlink_os_handle_t shm_fd;
  texlink_shm_t *shm;
  texlink_frame_t **frames;
  uint32_t frame_count;
  texlink_backend_t backend_type;
  texlink_state_t state;
  int last_error;
  int write_idx;
  int is_registered;
  char shm_name[64];
  char name[TEXLINK_NAME_MAX];
  char path[TEXLINK_SOCKET_PATH_MAX];
};

struct texlink_client {
  texlink_session_t *session;
  texlink_backend_t backend_type;
  texlink_state_t state;
  int last_error;
  int timeout_ms;
  char name[TEXLINK_NAME_MAX];
  char path[TEXLINK_SOCKET_PATH_MAX];
};

/* socket_*.c */
int texlink_send_ipc_handles(texlink_socket_t sock,
                             const texlink_ipc_handle_t *handles, int count);
int texlink_recv_ipc_handles(texlink_socket_t sock,
                             texlink_ipc_handle_t *handles, int count);
texlink_socket_t texlink_socket_bind(const char *path);
texlink_socket_t texlink_socket_connect(const char *path);
int texlink_socket_accept(texlink_socket_t server, texlink_socket_t *out_client);
int texlink_socket_poll(texlink_socket_t sock, int timeout_ms);
void texlink_socket_close(texlink_socket_t sock);
int texlink_socket_send(texlink_socket_t sock, const void *data, size_t size);
int texlink_socket_recv(texlink_socket_t sock, void *data, size_t size);
int texlink_send_frame(texlink_socket_t sock, const texlink_frame_msg_t *msg,
                       int sync_fd);
int texlink_recv_frame(texlink_socket_t sock, texlink_frame_msg_t *msg,
                       int *sync_fd);

/* sync_*.c */
int texlink_export_sync_file(int dma_fd);
int texlink_wait_sync_file(int sync_fd, int timeout_ms);

/* registry_*.c */
void texlink_registry_announce(const char *name, const char *path);
int texlink_registry_unregister(const char *name);

/* platform_*.c */
texlink_os_handle_t texlink_shm_create(const char *name, size_t size);
texlink_os_handle_t texlink_shm_open(const char *name);
void *texlink_shm_map(texlink_os_handle_t handle, size_t size);
int texlink_shm_map_failed(void *ptr);
void texlink_shm_unmap(void *ptr, size_t size);
void texlink_shm_close(texlink_os_handle_t handle);
void texlink_shm_unlink(const char *name);

void texlink_make_shm_name(const char *path, char *out, size_t len);
void texlink_make_socket_dir(const char *path);
void texlink_cleanup_socket_path(const char *path);
void texlink_default_path_for_name(const char *name, char *path, size_t len);

int texlink_native_handle_type_is_ipc(texlink_native_handle_type_t type);
texlink_native_handle_type_t texlink_default_native_handle_type(void);
void texlink_frame_init_received(texlink_frame_t *frame);
int texlink_frame_recv_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type);
int texlink_frame_send_native_handle(texlink_socket_t sock,
                                     texlink_frame_t *frame,
                                     texlink_native_handle_type_t type);
int texlink_frame_recv_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type);
int texlink_frame_send_sync_handle(texlink_socket_t sock,
                                   texlink_frame_t *frame,
                                   texlink_native_handle_type_t type);
void texlink_frame_close_ipc_handle(texlink_frame_t *frame);
void texlink_frame_close_sync_handle(texlink_frame_t *frame);
void texlink_frame_unmap_ipc_view(texlink_frame_t *frame);
void texlink_close_sync_fd(int sync_fd);
