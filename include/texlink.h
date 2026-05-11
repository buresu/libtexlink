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
} texlink_backend_t;

typedef enum {
  TEXLINK_FRAME_TYPE_TEXTURE_2D,
  TEXLINK_FRAME_TYPE_TEXTURE_3D,
  TEXLINK_FRAME_TYPE_TEXTURE_CUBE,
  TEXLINK_FRAME_TYPE_VERTEX_BUFFER,
  TEXLINK_FRAME_TYPE_COMPUTE_BUFFER,
  TEXLINK_FRAME_TYPE_RAW,
} texlink_frame_type_t;

#define TEXLINK_FOURCC_CODE(a, b, c, d)                                        \
  ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) |              \
   ((uint32_t)(d) << 24))

typedef enum {
  TEXLINK_FRAME_FORMAT_UNKNOWN = 0,
  TEXLINK_FRAME_FORMAT_R8 = TEXLINK_FOURCC_CODE('R', '8', ' ', ' '),
  TEXLINK_FRAME_FORMAT_ARGB8888 = TEXLINK_FOURCC_CODE('A', 'R', '2', '4'),
  TEXLINK_FRAME_FORMAT_XRGB8888 = TEXLINK_FOURCC_CODE('X', 'R', '2', '4'),
  TEXLINK_FRAME_FORMAT_ABGR8888 = TEXLINK_FOURCC_CODE('A', 'B', '2', '4'),
  TEXLINK_FRAME_FORMAT_XBGR8888 = TEXLINK_FOURCC_CODE('X', 'B', '2', '4'),
} texlink_frame_format_t;

typedef enum {
  TEXLINK_ACCESS_READ = 1u << 0,
  TEXLINK_ACCESS_WRITE = 1u << 1,
} texlink_access_t;

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

typedef struct texlink_frame texlink_frame_t;
typedef struct texlink_server texlink_server_t;
typedef struct texlink_client texlink_client_t;

typedef struct {
  uint32_t version;

  texlink_frame_type_t type;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t format;
  uint64_t size;
  uint32_t flags;
} texlink_frame_desc_t;

typedef struct {
  uint32_t version;

  const char *name;
  const char *path;

  texlink_backend_t backend;

  texlink_frame_t **frames;
  uint32_t frame_count;

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
texlink_frame_t *texlink_server_begin_frame(texlink_server_t *server);
int texlink_server_end_frame(texlink_server_t *server, texlink_frame_t *frame);
int texlink_server_client_count(texlink_server_t *server);
void texlink_server_destroy(texlink_server_t *server);

/* Client API */
texlink_client_t *texlink_client_create(const texlink_client_desc_t *desc);
int texlink_client_connect(texlink_client_t *client);
void texlink_client_disconnect(texlink_client_t *client);
void texlink_client_destroy(texlink_client_t *client);
texlink_frame_t *texlink_client_acquire_frame(texlink_client_t *client);
void texlink_client_release_frame(texlink_client_t *client,
                                  texlink_frame_t *frame);
uint32_t texlink_client_frame_count(texlink_client_t *client);
texlink_frame_t *texlink_client_frame(texlink_client_t *client, uint32_t idx);
texlink_meta_t texlink_client_meta(texlink_client_t *client);

/* Frame API */
texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc);
void texlink_frame_destroy(texlink_frame_t *frame);
texlink_meta_t texlink_frame_meta(texlink_frame_t *frame);
int texlink_frame_index(texlink_frame_t *frame);

/* Native handle accessors */
int texlink_frame_get_dma_fd(texlink_frame_t *frame);
int texlink_frame_get_sync_fd(texlink_frame_t *frame);

/*
 * Host access to a DMA-BUF-backed frame. The implementation maps the frame
 * and performs any required CPU cache coherency synchronization.
 */
void *texlink_frame_begin_access(texlink_frame_t *frame, uint32_t access);
int texlink_frame_end_access(texlink_frame_t *frame);

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
