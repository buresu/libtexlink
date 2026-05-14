#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_d3d11_texture_frame texlink_d3d11_texture_frame_t;

typedef struct {
  uint32_t version;
  ID3D11Device *device;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  UINT bind_flags;
  UINT misc_flags;
} texlink_d3d11_texture_frame_desc_t;

typedef struct {
  uint32_t version;
  ID3D11Device *device;
  texlink_frame_t *frame;
  uint32_t flags;
} texlink_d3d11_import_desc_t;

typedef struct {
  uint32_t version;
  ID3D11Texture2D *texture;
  HANDLE shared_handle;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint64_t size;
  uint32_t flags;
} texlink_d3d11_wrap_texture_desc_t;

DXGI_FORMAT texlink_d3d11_format(uint32_t texlink_format);

texlink_d3d11_texture_frame_t *texlink_d3d11_texture_frame_create(
    const texlink_d3d11_texture_frame_desc_t *desc);
texlink_d3d11_texture_frame_t *texlink_d3d11_texture_frame_import(
    const texlink_d3d11_import_desc_t *desc);
texlink_frame_t *texlink_d3d11_texture_frame_frame(
    texlink_d3d11_texture_frame_t *texture_frame);
ID3D11Texture2D *texlink_d3d11_texture_frame_texture(
    texlink_d3d11_texture_frame_t *texture_frame);
ID3D11Device *texlink_d3d11_texture_frame_device(
    texlink_d3d11_texture_frame_t *texture_frame);
void texlink_d3d11_texture_frame_destroy(
    texlink_d3d11_texture_frame_t *texture_frame);

texlink_frame_t *texlink_d3d11_frame_wrap_texture(
    const texlink_d3d11_wrap_texture_desc_t *desc);

const char *texlink_d3d11_last_error_string(void);

#ifdef __cplusplus
}
#endif
