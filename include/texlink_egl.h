#pragma once

#include <EGL/egl.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_egl_image texlink_egl_image_t;
typedef struct texlink_egl_texture_frame texlink_egl_texture_frame_t;

typedef struct {
  uint32_t version;
  EGLDisplay display;
  texlink_frame_t *frame;
} texlink_egl_import_desc_t;

typedef struct {
  uint32_t version;
  EGLDisplay display;
  uint32_t width;
  uint32_t height;
  uint32_t format;
} texlink_egl_texture_frame_desc_t;

typedef struct {
  uint32_t version;
  EGLDisplay display;
  EGLContext context;
  unsigned int texture;
  uint32_t width;
  uint32_t height;
  uint32_t type;
  uint32_t format;
} texlink_egl_wrap_texture_desc_t;

texlink_egl_image_t *
texlink_egl_image_import(const texlink_egl_import_desc_t *desc);
EGLImage texlink_egl_image_handle(texlink_egl_image_t *image);
int texlink_egl_image_bind_texture_2d(texlink_egl_image_t *image,
                                      unsigned int texture);
void texlink_egl_image_destroy(texlink_egl_image_t *image);

texlink_egl_texture_frame_t *
texlink_egl_texture_frame_create(const texlink_egl_texture_frame_desc_t *desc);
texlink_frame_t *
texlink_egl_texture_frame_frame(texlink_egl_texture_frame_t *texture_frame);
unsigned int
texlink_egl_texture_frame_texture(texlink_egl_texture_frame_t *texture_frame);
void texlink_egl_texture_frame_destroy(
    texlink_egl_texture_frame_t *texture_frame);

texlink_frame_t *
texlink_egl_frame_wrap_texture_2d(const texlink_egl_wrap_texture_desc_t *desc);

#ifdef __cplusplus
}
#endif
