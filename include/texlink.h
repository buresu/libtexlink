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
  TEXLINK_BACKEND_WGL,
  TEXLINK_BACKEND_VULKAN,
  TEXLINK_BACKEND_D3D11,
  TEXLINK_BACKEND_D3D12,
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
  TEXLINK_STATE_CLOSED = 0,
  TEXLINK_STATE_CREATED,
  TEXLINK_STATE_LISTENING,
  TEXLINK_STATE_CONNECTING,
  TEXLINK_STATE_CONNECTED,
  TEXLINK_STATE_DISCONNECTED,
  TEXLINK_STATE_ERROR,
} texlink_state_t;

typedef struct {
  uint32_t backend_type;
  uint32_t handle_type;
  uint32_t frame_type;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t stride;
  uint32_t format;
  uint64_t modifier;
  uint64_t size;
  uint64_t frame_id;
  uint32_t sync_handle_type;
  uint32_t _reserved;
  uint64_t sync_value;
} texlink_meta_t;

typedef struct texlink_frame texlink_frame_t;
typedef struct texlink_server texlink_server_t;
typedef struct texlink_client texlink_client_t;

typedef struct {
  texlink_frame_type_t frame_type;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t format;
  uint32_t stride;
  uint64_t size;
} texlink_frame_desc_t;

typedef enum {
  TEXLINK_NATIVE_HANDLE_UNKNOWN = 0,

  TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
  TEXLINK_NATIVE_HANDLE_SYNC_FD,
  TEXLINK_NATIVE_HANDLE_OPAQUE_FD,

  TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE,
  TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE,
  TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE,
  TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
} texlink_native_handle_type_t;

typedef enum {
  TEXLINK_NATIVE_HANDLE_FLAG_NONE = 0,
  TEXLINK_NATIVE_HANDLE_FLAG_BORROWED = 1u << 0,
  TEXLINK_NATIVE_HANDLE_FLAG_OWNED = 1u << 1,
} texlink_native_handle_flags_t;

typedef struct {
  texlink_native_handle_type_t handle_type;
  texlink_native_handle_flags_t flags;
  union {
    int fd;
    void *ptr;
    uint64_t u64;
  } value;
} texlink_native_handle_t;

typedef struct {
  texlink_backend_t backend_type;
  texlink_frame_type_t frame_type;
  texlink_native_handle_t handle;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t format;
  uint32_t stride;
  uint64_t modifier;
  uint64_t size;
} texlink_frame_native_desc_t;

typedef enum {
  TEXLINK_MAP_READ = 1u << 0,
  TEXLINK_MAP_WRITE = 1u << 1,
} texlink_map_flags_t;

typedef struct {
  texlink_map_flags_t flags;
  uint64_t offset;
  uint64_t size;
} texlink_map_desc_t;

typedef struct {
  void *data;
  uint64_t size;
  uint32_t stride;
} texlink_mapping_t;

typedef struct {
  texlink_backend_t backend_type;
  const char *name;
  const char *path;
  texlink_frame_t **frames;
  uint32_t frame_count;
} texlink_server_desc_t;

typedef struct {
  texlink_backend_t backend_type;
  const char *name;
  const char *path;
  int timeout_ms;
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
texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc);
void texlink_frame_destroy(texlink_frame_t *frame);
texlink_meta_t texlink_frame_meta(texlink_frame_t *frame);
int texlink_frame_index(texlink_frame_t *frame);

int texlink_frame_should_flip_y(texlink_backend_t producer,
                                texlink_backend_t consumer);

/* Frame native handle API */
int texlink_frame_get_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle);
int texlink_frame_dup_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle);
int texlink_frame_set_sync_native_handle(texlink_frame_t *frame,
                                         const texlink_native_handle_t *handle,
                                         uint64_t value);
int texlink_frame_get_sync_native_handle(texlink_frame_t *frame,
                                         texlink_native_handle_type_t type,
                                         texlink_native_handle_t *out_handle,
                                         uint64_t *out_value);
int texlink_frame_set_sync_value(texlink_frame_t *frame, uint64_t value);
uint64_t texlink_frame_sync_value(texlink_frame_t *frame);

/* Native handle API */
int texlink_native_handle_close(texlink_native_handle_t *handle);

/* Frame mapping API */
int texlink_frame_map(texlink_frame_t *frame, const texlink_map_desc_t *desc,
                      texlink_mapping_t *out_mapping);
int texlink_frame_unmap(texlink_frame_t *frame);
int texlink_frame_is_mapped(texlink_frame_t *frame);

/* Frame CPU access API */
int texlink_frame_cpu_begin(texlink_frame_t *frame,
                            const texlink_map_desc_t *desc);
int texlink_frame_cpu_end(texlink_frame_t *frame,
                          const texlink_map_desc_t *desc);

/* Name-based discovery */
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
