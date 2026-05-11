#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TEXLINK_BACKEND_UNKNOWN = 0,
  TEXLINK_BACKEND_CPU,
  TEXLINK_BACKEND_EGL,
  TEXLINK_BACKEND_VULKAN,
  TEXLINK_BACKEND_CUDA,
  TEXLINK_BACKEND_METAL,
  TEXLINK_BACKEND_D3D11,
  TEXLINK_BACKEND_D3D12,
} texlink_backend_t;

typedef enum {
  TEXLINK_TYPE_TEXTURE_2D,
  TEXLINK_TYPE_TEXTURE_3D,
  TEXLINK_TYPE_TEXTURE_CUBE,
  TEXLINK_TYPE_VERTEX_BUFFER,
  TEXLINK_TYPE_COMPUTE_BUFFER,
  TEXLINK_TYPE_RAW,
} texlink_type_t;

typedef enum {
  TEXLINK_STATE_CLOSED = 0,
  TEXLINK_STATE_CREATED,
  TEXLINK_STATE_LISTENING,
  TEXLINK_STATE_CONNECTING,
  TEXLINK_STATE_CONNECTED,
  TEXLINK_STATE_DISCONNECTED,
  TEXLINK_STATE_ERROR,
} texlink_state_t;

typedef struct {
  uint32_t backend;
  uint32_t type;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t stride;
  uint32_t format;
  uint64_t modifier;
  uint32_t size;
  uint64_t frame_id;
} texlink_meta_t;

typedef struct texlink_buf texlink_buf_t;
typedef struct texlink_server texlink_server_t;
typedef struct texlink_client texlink_client_t;

typedef struct {
  uint32_t version;

  const char *name;
  const char *path;

  texlink_backend_t backend;

  texlink_buf_t **bufs;
  uint32_t buffer_count;

  uint32_t flags;
} texlink_server_desc_t;

typedef struct {
  uint32_t version;

  const char *name;
  const char *path;

  texlink_backend_t backend;

  int timeout_ms;
  uint32_t flags;
} texlink_client_desc_t;

/* Server API */
texlink_server_t *texlink_server_create(const texlink_server_desc_t *desc);
int texlink_server_start(texlink_server_t *server);
int texlink_server_poll(texlink_server_t *server);
int texlink_server_begin_frame(texlink_server_t *server);
int texlink_server_end_frame(texlink_server_t *server, int idx);
int texlink_server_client_count(texlink_server_t *server);
void texlink_server_destroy(texlink_server_t *server);

/* Client API */
texlink_client_t *texlink_client_create(const texlink_client_desc_t *desc);
int texlink_client_connect(texlink_client_t *client);
void texlink_client_disconnect(texlink_client_t *client);
void texlink_client_destroy(texlink_client_t *client);
int texlink_client_acquire_frame(texlink_client_t *client);
void texlink_client_release_frame(texlink_client_t *client, int idx);
texlink_buf_t *texlink_client_buf(texlink_client_t *client, int idx);
texlink_meta_t texlink_client_meta(texlink_client_t *client);

/* Buffer API */
texlink_buf_t *texlink_buf_alloc(uint32_t width, uint32_t height,
                                 uint32_t format, texlink_type_t type);
void texlink_buf_free(texlink_buf_t *buf);
texlink_meta_t texlink_buf_meta(texlink_buf_t *buf);

/* Native handle accessors */
int texlink_buf_get_dma_fd(texlink_buf_t *buf);
int texlink_buf_get_sync_fd(texlink_buf_t *buf);
void *texlink_buf_map_cpu(texlink_buf_t *buf);

/*
 * CPU cache coherency sync (required on ARM and other non-coherent systems).
 * Call texlink_buf_cpu_begin() before any CPU read/write of a mapped DMA-BUF
 * region, and texlink_buf_cpu_end() when done. write=1 for writes, write=0 for
 * reads.
 */
int texlink_buf_cpu_begin(texlink_buf_t *buf, int write);
int texlink_buf_cpu_end(texlink_buf_t *buf, int write);

/* Name-based discovery (Spout-style registry) */
#define TEXLINK_NAME_MAX 64

int texlink_registry_list(char (*names)[TEXLINK_NAME_MAX], int max);
int texlink_registry_resolve(const char *name, char *path, size_t path_size);

/* State and error API */
texlink_state_t texlink_server_state(texlink_server_t *server);
texlink_state_t texlink_client_state(texlink_client_t *client);
int texlink_server_last_error(texlink_server_t *server);
int texlink_client_last_error(texlink_client_t *client);
const char *texlink_error_string(int err);

#ifdef __cplusplus
}
#endif
