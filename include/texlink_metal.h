#pragma once

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_metal_texture_frame texlink_metal_texture_frame_t;

typedef struct {
  void *device;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t usage;
} texlink_metal_texture_frame_desc_t;

typedef struct {
  void *device;
  texlink_frame_t *frame;
  uint32_t usage;
} texlink_metal_import_desc_t;

typedef struct {
  void *texture;
  uint32_t format;
} texlink_metal_wrap_texture_desc_t;

texlink_metal_texture_frame_t *texlink_metal_texture_frame_create(
    const texlink_metal_texture_frame_desc_t *desc);
texlink_metal_texture_frame_t *texlink_metal_texture_frame_import(
    const texlink_metal_import_desc_t *desc);
texlink_frame_t *texlink_metal_texture_frame_frame(
    texlink_metal_texture_frame_t *texture_frame);
void *texlink_metal_texture_frame_texture(
    texlink_metal_texture_frame_t *texture_frame);
void *texlink_metal_texture_frame_device(
    texlink_metal_texture_frame_t *texture_frame);
void texlink_metal_texture_frame_destroy(
    texlink_metal_texture_frame_t *texture_frame);

texlink_frame_t *texlink_metal_frame_wrap_texture(
    const texlink_metal_wrap_texture_desc_t *desc);

const char *texlink_metal_last_error_string(void);

#ifdef __cplusplus
}
#endif
