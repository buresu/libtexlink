#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  DMABL_TYPE_TEXTURE_2D,
  DMABL_TYPE_TEXTURE_3D,
  DMABL_TYPE_TEXTURE_CUBE,
  DMABL_TYPE_VERTEX_BUFFER,
  DMABL_TYPE_COMPUTE_BUFFER,
  DMABL_TYPE_RAW,
} dmabl_type_t;

typedef enum {
  DMABL_BUFFERING_SINGLE = 1,
  DMABL_BUFFERING_DOUBLE = 2,
  DMABL_BUFFERING_TRIPLE = 3,
} dmabl_buffering_t;

typedef struct {
  uint32_t type;
  uint32_t width;
  uint32_t height;
  uint32_t depth;
  uint32_t stride;
  uint32_t format;
  uint64_t modifier;
  uint32_t size;
  uint64_t frame_id;
} dmabl_meta_t;

typedef struct dmabl_buf dmabl_buf_t;
typedef struct dmabl_session dmabl_session_t;

/* Buffer alloc/free */
dmabl_buf_t *dmabl_alloc(uint32_t w, uint32_t h, uint32_t format,
                         dmabl_type_t type);
void dmabl_free(dmabl_buf_t *buf);

/* Session management */
dmabl_session_t *dmabl_serve(const char *path, dmabl_buf_t **bufs,
                             dmabl_buffering_t buffering);
dmabl_session_t *dmabl_serve_named(const char *name, dmabl_buf_t **bufs,
                                   dmabl_buffering_t buffering);
dmabl_session_t *dmabl_connect(const char *path);
void dmabl_session_close(dmabl_session_t *s);

/* Buffer access within session */
dmabl_buf_t *dmabl_session_buf(dmabl_session_t *s, int idx);
dmabl_meta_t dmabl_session_meta(dmabl_session_t *s);

/* Producer API */
int dmabl_producer_begin(dmabl_session_t *s);
void dmabl_producer_end(dmabl_session_t *s, int idx);

/* Consumer API */
int dmabl_consumer_acquire(dmabl_session_t *s);
void dmabl_consumer_release(dmabl_session_t *s, int idx);

/* Native handle accessors */
int dmabl_get_dma_fd(dmabl_buf_t *buf);
int dmabl_get_sync_fd(dmabl_buf_t *buf);
void *dmabl_map_cpu(dmabl_buf_t *buf);

/*
 * CPU cache coherency sync (required on ARM and other non-coherent systems).
 * Call dmabl_cpu_begin() before any CPU read/write of a mapped DMA-BUF region,
 * and dmabl_cpu_end() when done. write=1 for writes, write=0 for reads.
 */
int dmabl_cpu_begin(dmabl_buf_t *buf, int write);
int dmabl_cpu_end(dmabl_buf_t *buf, int write);

/* Name-based discovery (Spout-style registry) */
#define DMABL_NAME_MAX 64

int dmabl_register(dmabl_session_t *s, const char *name);
void dmabl_unregister(dmabl_session_t *s);
int dmabl_list_sessions(char (*names)[DMABL_NAME_MAX], int max);
dmabl_session_t *dmabl_connect_by_name(const char *name);

#ifdef __cplusplus
}
#endif
