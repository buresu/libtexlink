#pragma once

#include "dmabuflink.h"

#include <stdatomic.h>
#include <sys/types.h>

/* GBM forward declarations to avoid hard include in this header */
struct gbm_device;
struct gbm_bo;

#define DMABL_MAX_BUFS 3
#define DMABL_SHM_PREFIX "/dmabl_"
#define DMABL_PROTO_VER 1
#define DMABL_NAME_MAX 64
#define DMABL_MAX_SESSIONS 64
#define DMABL_REGISTRY_PATH "/tmp/dmabl/.registry"
#define DMABL_REGISTRY_MAGIC 0x444D424Cu /* DMBL */

struct dmabl_buf {
  int dma_fd;
  int sync_fd;
  void *map_ptr;
  size_t size;
  dmabl_meta_t meta;

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
  dmabl_meta_t meta;
  uint8_t _pad[48]; /* pad to 128 bytes */
} dmabl_shm_t;

/* Handshake sent over socket at connect time */
typedef struct {
  uint32_t version;
  uint32_t buf_count;
  dmabl_buffering_t buffering;
  uint32_t _pad;
  dmabl_meta_t meta;
  char shm_name[64];
} dmabl_handshake_t;

/* Per-frame notification sent by producer to consumer */
typedef struct {
  uint64_t frame_id;
  uint32_t buf_idx;
  uint32_t has_sync_fd; /* 1 if SCM_RIGHTS carries sync_fd */
} dmabl_frame_msg_t;

/* Registry entry stored in DMABL_REGISTRY_PATH */
typedef struct {
  char name[DMABL_NAME_MAX];
  char path[108]; /* sockaddr_un.sun_path max */
  pid_t pid;
  uint32_t active;
} dmabl_registry_entry_t;

typedef struct {
  uint32_t magic;
  uint32_t _pad;
  dmabl_registry_entry_t entries[DMABL_MAX_SESSIONS];
} dmabl_registry_t;

struct dmabl_session {
  int sock_fd;
  int shm_fd;
  dmabl_shm_t *shm;
  dmabl_buf_t *bufs[DMABL_MAX_BUFS];
  int buf_count;
  dmabl_buffering_t buffering;
  int is_producer;
  int write_idx; /* producer rotation cursor */
  int is_registered;
  uint64_t last_frame_id; /* updated by consumer_acquire from socket msg */
  char shm_name[64];
  char sock_path[108];
  char reg_name[DMABL_NAME_MAX];
};

/* socket.c */
int dmabl_send_fds(int sock, const int *fds, int nfds);
int dmabl_recv_fds(int sock, int *fds, int nfds);
int dmabl_socket_bind(const char *path);
int dmabl_socket_connect(const char *path);
int dmabl_send_frame(int sock, const dmabl_frame_msg_t *msg, int sync_fd);
int dmabl_recv_frame(int sock, dmabl_frame_msg_t *msg, int *sync_fd);

/* sync.c */
int dmabl_export_sync_file(int dma_fd);
int dmabl_wait_sync_file(int sync_fd, int timeout_ms);
int dmabl_cpu_begin(dmabl_buf_t *buf, int write);
int dmabl_cpu_end(dmabl_buf_t *buf, int write);

/* registry.c */
void dmabl_registry_announce(const char *name, const char *path);
int dmabl_register(dmabl_session_t *s, const char *name);
void dmabl_unregister(dmabl_session_t *s);
