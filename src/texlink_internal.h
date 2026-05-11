#pragma once

#include "texlink.h"

#include <stdatomic.h>
#include <sys/types.h>

/* GBM forward declarations to avoid hard include in this header */
struct gbm_device;
struct gbm_bo;

#define TEXLINK_MAX_BUFS 3
#define TEXLINK_SHM_PREFIX "/texlink_"
#define TEXLINK_PROTO_VER 1
#define TEXLINK_NAME_MAX 64
#define TEXLINK_MAX_CLIENTS 16
#define TEXLINK_MAX_SESSIONS 64
#define TEXLINK_REGISTRY_PATH "/tmp/texlink/.registry"
#define TEXLINK_REGISTRY_MAGIC 0x544C4E4Bu /* TLNK */

typedef enum {
  TEXLINK_BUFFERING_SINGLE = 1,
  TEXLINK_BUFFERING_DOUBLE = 2,
  TEXLINK_BUFFERING_TRIPLE = 3,
} texlink_buffering_t;

typedef struct texlink_session texlink_session_t;

struct texlink_buf {
  int dma_fd;
  int sync_fd;
  void *map_ptr;
  size_t size;
  texlink_meta_t meta;

  /* GBM-backed (texture types) — NULL for dma_heap or consumer-side bufs */
  struct gbm_bo *bo;
  struct gbm_device *gbm;
  int drm_fd;
};

/* Shared memory layout — must be identical in producer and consumer */
typedef struct {
  _Atomic uint64_t frame_id;
  _Atomic uint32_t current_idx;
  uint32_t buf_count;
  texlink_meta_t meta;
  uint8_t _pad[48]; /* pad to 128 bytes */
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
} texlink_frame_msg_t;

/* Registry entry stored in TEXLINK_REGISTRY_PATH */
typedef struct {
  char name[TEXLINK_NAME_MAX];
  char path[108]; /* sockaddr_un.sun_path max */
  pid_t pid;
  uint32_t active;
} texlink_registry_entry_t;

typedef struct {
  uint32_t magic;
  uint32_t _pad;
  texlink_registry_entry_t entries[TEXLINK_MAX_SESSIONS];
} texlink_registry_t;

struct texlink_session {
  int sock_fd;
  int shm_fd;
  texlink_shm_t *shm;
  texlink_buf_t *bufs[TEXLINK_MAX_BUFS];
  int buf_count;
  texlink_buffering_t buffering;
  int is_producer;
  int write_idx; /* producer rotation cursor */
  int is_registered;
  uint64_t last_frame_id; /* updated by consumer_acquire from socket msg */
  char shm_name[64];
  char sock_path[108];
  char reg_name[TEXLINK_NAME_MAX];
};

struct texlink_server {
  int listen_fd;
  int client_fds[TEXLINK_MAX_CLIENTS];
  int shm_fd;
  texlink_shm_t *shm;
  texlink_buf_t **bufs;
  uint32_t buffer_count;
  texlink_backend_t backend;
  texlink_state_t state;
  int last_error;
  int write_idx;
  int is_registered;
  uint32_t flags;
  char shm_name[64];
  char name[TEXLINK_NAME_MAX];
  char path[108];
};

struct texlink_client {
  texlink_session_t *session;
  texlink_backend_t backend;
  texlink_state_t state;
  int last_error;
  int timeout_ms;
  uint32_t flags;
  char name[TEXLINK_NAME_MAX];
  char path[108];
};

/* socket.c */
int texlink_send_fds(int sock, const int *fds, int nfds);
int texlink_recv_fds(int sock, int *fds, int nfds);
int texlink_socket_bind(const char *path);
int texlink_socket_connect(const char *path);
int texlink_send_frame(int sock, const texlink_frame_msg_t *msg, int sync_fd);
int texlink_recv_frame(int sock, texlink_frame_msg_t *msg, int *sync_fd);

/* sync.c */
int texlink_export_sync_file(int dma_fd);
int texlink_wait_sync_file(int sync_fd, int timeout_ms);

/* registry.c */
void texlink_registry_announce(const char *name, const char *path);
int texlink_registry_unregister(const char *name);
