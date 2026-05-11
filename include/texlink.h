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
  TEXLINK_BUFFERING_SINGLE = 1,
  TEXLINK_BUFFERING_DOUBLE = 2,
  TEXLINK_BUFFERING_TRIPLE = 3,
} texlink_buffering_t;

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
typedef struct texlink_session texlink_session_t;

/* Buffer alloc/free */
texlink_buf_t *texlink_alloc(uint32_t w, uint32_t h, uint32_t format,
                         texlink_type_t type, texlink_backend_t backend);
void texlink_free(texlink_buf_t *buf);

/* Session management */
texlink_session_t *texlink_serve(const char *path, texlink_buf_t **bufs,
                             texlink_buffering_t buffering);
texlink_session_t *texlink_serve_named(const char *name, texlink_buf_t **bufs,
                                   texlink_buffering_t buffering);
texlink_session_t *texlink_connect(const char *path);
void texlink_session_close(texlink_session_t *s);

/* Buffer access within session */
texlink_buf_t *texlink_session_buf(texlink_session_t *s, int idx);
texlink_meta_t texlink_session_meta(texlink_session_t *s);

/* Producer API */
int texlink_producer_begin(texlink_session_t *s);
void texlink_producer_end(texlink_session_t *s, int idx);

/* Consumer API */
int texlink_consumer_acquire(texlink_session_t *s);
void texlink_consumer_release(texlink_session_t *s, int idx);

/* Native handle accessors */
int texlink_get_dma_fd(texlink_buf_t *buf);
int texlink_get_sync_fd(texlink_buf_t *buf);
void *texlink_map_cpu(texlink_buf_t *buf);

/*
 * CPU cache coherency sync (required on ARM and other non-coherent systems).
 * Call texlink_cpu_begin() before any CPU read/write of a mapped DMA-BUF region,
 * and texlink_cpu_end() when done. write=1 for writes, write=0 for reads.
 */
int texlink_cpu_begin(texlink_buf_t *buf, int write);
int texlink_cpu_end(texlink_buf_t *buf, int write);

/* Name-based discovery (Spout-style registry) */
#define TEXLINK_NAME_MAX 64

int texlink_register(texlink_session_t *s, const char *name);
void texlink_unregister(texlink_session_t *s);
int texlink_list_sessions(char (*names)[TEXLINK_NAME_MAX], int max);
texlink_session_t *texlink_connect_by_name(const char *name);

#ifdef __cplusplus
}
#endif
