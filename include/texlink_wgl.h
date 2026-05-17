#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_wgl_texture_frame texlink_wgl_texture_frame_t;

typedef struct {
  uint32_t width;
  uint32_t height;
  uint32_t format;
} texlink_wgl_texture_frame_desc_t;

typedef struct {
  texlink_frame_t *frame;
} texlink_wgl_import_desc_t;

texlink_wgl_texture_frame_t *
texlink_wgl_texture_frame_create(const texlink_wgl_texture_frame_desc_t *desc);

texlink_wgl_texture_frame_t *
texlink_wgl_texture_frame_import(const texlink_wgl_import_desc_t *desc);

texlink_frame_t *
texlink_wgl_texture_frame_frame(texlink_wgl_texture_frame_t *texture_frame);

unsigned int
texlink_wgl_texture_frame_texture(texlink_wgl_texture_frame_t *texture_frame);

int texlink_wgl_texture_frame_lock(texlink_wgl_texture_frame_t *texture_frame);
int texlink_wgl_texture_frame_unlock(texlink_wgl_texture_frame_t *texture_frame);

const char *texlink_wgl_last_error_string(void);

void texlink_wgl_texture_frame_destroy(
    texlink_wgl_texture_frame_t *texture_frame);

#ifdef __cplusplus
}
#endif
