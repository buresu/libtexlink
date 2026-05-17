#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_d3d12_texture_frame texlink_d3d12_texture_frame_t;
typedef struct texlink_d3d12_fence_frame texlink_d3d12_fence_frame_t;

typedef struct {
  ID3D12Device *device;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  D3D12_RESOURCE_FLAGS resource_flags;
  D3D12_RESOURCE_STATES initial_state;
  D3D12_HEAP_TYPE heap_type;
} texlink_d3d12_texture_frame_desc_t;

typedef struct {
  ID3D12Device *device;
  texlink_frame_t *frame;
} texlink_d3d12_import_desc_t;

typedef struct {
  ID3D12Device *device;
  ID3D12Resource *resource;
  HANDLE shared_handle;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t stride;
  uint64_t size;
} texlink_d3d12_wrap_resource_desc_t;

typedef struct {
  ID3D12Device *device;
  uint64_t initial_value;
  D3D12_FENCE_FLAGS flags;
} texlink_d3d12_fence_frame_desc_t;

typedef struct {
  ID3D12Device *device;
  texlink_frame_t *frame;
} texlink_d3d12_fence_import_desc_t;

DXGI_FORMAT texlink_d3d12_format(uint32_t texlink_format);

texlink_d3d12_texture_frame_t *texlink_d3d12_texture_frame_create(
    const texlink_d3d12_texture_frame_desc_t *desc);
texlink_d3d12_texture_frame_t *texlink_d3d12_texture_frame_import(
    const texlink_d3d12_import_desc_t *desc);
texlink_frame_t *texlink_d3d12_texture_frame_frame(
    texlink_d3d12_texture_frame_t *texture_frame);
ID3D12Resource *texlink_d3d12_texture_frame_resource(
    texlink_d3d12_texture_frame_t *texture_frame);
ID3D12Device *texlink_d3d12_texture_frame_device(
    texlink_d3d12_texture_frame_t *texture_frame);
void texlink_d3d12_texture_frame_destroy(
    texlink_d3d12_texture_frame_t *texture_frame);

texlink_frame_t *texlink_d3d12_frame_wrap_resource(
    const texlink_d3d12_wrap_resource_desc_t *desc);

texlink_d3d12_fence_frame_t *
texlink_d3d12_fence_frame_create(const texlink_d3d12_fence_frame_desc_t *desc);
texlink_d3d12_fence_frame_t *
texlink_d3d12_fence_frame_import(const texlink_d3d12_fence_import_desc_t *desc);
texlink_frame_t *
texlink_d3d12_fence_frame_frame(texlink_d3d12_fence_frame_t *fence_frame);
ID3D12Fence *
texlink_d3d12_fence_frame_fence(texlink_d3d12_fence_frame_t *fence_frame);
void texlink_d3d12_fence_frame_destroy(
    texlink_d3d12_fence_frame_t *fence_frame);

const char *texlink_d3d12_last_error_string(void);

#ifdef __cplusplus
}
#endif
