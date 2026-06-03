#pragma once

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_cgl_texture_frame texlink_cgl_texture_frame_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t target;
} texlink_cgl_texture_frame_desc_t;

typedef struct {
  texlink_frame_t *frame;
  uint32_t target;
} texlink_cgl_import_desc_t;

texlink_cgl_texture_frame_t *texlink_cgl_texture_frame_create(
    const texlink_cgl_texture_frame_desc_t *desc);
texlink_cgl_texture_frame_t *texlink_cgl_texture_frame_import(
    const texlink_cgl_import_desc_t *desc);
texlink_frame_t *texlink_cgl_texture_frame_frame(
    texlink_cgl_texture_frame_t *texture_frame);
unsigned int texlink_cgl_texture_frame_texture(
    texlink_cgl_texture_frame_t *texture_frame);
void texlink_cgl_texture_frame_destroy(
    texlink_cgl_texture_frame_t *texture_frame);

const char *texlink_cgl_last_error_string(void);

#ifdef __cplusplus
}
#endif
