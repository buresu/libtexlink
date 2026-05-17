#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi.h>
#include <texlink_d3d12.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct texlink_d3d12_texture_frame {
  texlink_frame_t *frame;
  ID3D12Device *device;
  ID3D12Resource *resource;
  int owns_frame;
};

struct texlink_d3d12_fence_frame {
  texlink_frame_t *frame;
  ID3D12Device *device;
  ID3D12Fence *fence;
  int owns_frame;
};

static const char *g_last_error = "no error";

static int fail(const char *message, int code) {
  g_last_error = message;
  return code;
}

const char *texlink_d3d12_last_error_string(void) {
  return g_last_error;
}

DXGI_FORMAT texlink_d3d12_format(uint32_t format) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return DXGI_FORMAT_R8_UNORM;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

static uint32_t default_stride(uint32_t width, uint32_t format) {
  return width * (format == TEXLINK_FRAME_FORMAT_R8 ? 1u : 4u);
}

static uint32_t texlink_format_from_dxgi(DXGI_FORMAT format) {
  switch (format) {
  case DXGI_FORMAT_R8_UNORM:
    return TEXLINK_FRAME_FORMAT_R8;
  case DXGI_FORMAT_R8G8B8A8_UNORM:
  case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    return TEXLINK_FRAME_FORMAT_ABGR8888;
  case DXGI_FORMAT_B8G8R8A8_UNORM:
  case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
  default:
    return TEXLINK_FRAME_FORMAT_ARGB8888;
  }
}

static void release_texture_frame(texlink_d3d12_texture_frame_t *tf) {
  if (!tf)
    return;
  if (tf->owns_frame)
    texlink_frame_destroy(tf->frame);
  if (tf->resource)
    tf->resource->lpVtbl->Release(tf->resource);
  if (tf->device)
    tf->device->lpVtbl->Release(tf->device);
}

static void release_fence_frame(texlink_d3d12_fence_frame_t *ff) {
  if (!ff)
    return;
  if (ff->owns_frame)
    texlink_frame_destroy(ff->frame);
  if (ff->fence)
    ff->fence->lpVtbl->Release(ff->fence);
  if (ff->device)
    ff->device->lpVtbl->Release(ff->device);
}

static int set_device(ID3D12Device **dst, ID3D12Device *device) {
  if (!dst || !device)
    return fail("D3D12 device is required", -EINVAL);
  *dst = device;
  (*dst)->lpVtbl->AddRef(*dst);
  return 0;
}

static int create_shared_handle(ID3D12Device *device, ID3D12DeviceChild *object,
                                HANDLE *out_handle) {
  HRESULT hr = device->lpVtbl->CreateSharedHandle(
      device, object, NULL, GENERIC_ALL, NULL, out_handle);
  return SUCCEEDED(hr) && *out_handle
             ? 0
             : fail("ID3D12Device::CreateSharedHandle failed", -EIO);
}

static int create_shared_resource(texlink_d3d12_texture_frame_t *tf,
                                  const texlink_d3d12_texture_frame_desc_t *desc,
                                  HANDLE *out_shared) {
  DXGI_FORMAT dxgi_format = texlink_d3d12_format(desc->format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return fail("unsupported D3D12 texture format", -EINVAL);

  D3D12_HEAP_PROPERTIES heap_props;
  memset(&heap_props, 0, sizeof(heap_props));
  heap_props.Type = desc->heap_type ? desc->heap_type : D3D12_HEAP_TYPE_DEFAULT;
  heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap_props.CreationNodeMask = 1;
  heap_props.VisibleNodeMask = 1;

  D3D12_RESOURCE_DESC rd;
  memset(&rd, 0, sizeof(rd));
  rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  rd.Alignment = 0;
  rd.Width = desc->width;
  rd.Height = desc->height;
  rd.DepthOrArraySize = 1;
  rd.MipLevels = 1;
  rd.Format = dxgi_format;
  rd.SampleDesc.Count = 1;
  rd.SampleDesc.Quality = 0;
  rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  rd.Flags = desc->resource_flags;

  HRESULT hr = tf->device->lpVtbl->CreateCommittedResource(
      tf->device, &heap_props, D3D12_HEAP_FLAG_SHARED, &rd,
      desc->initial_state, NULL, &IID_ID3D12Resource, (void **)&tf->resource);
  if (FAILED(hr) || !tf->resource)
    return fail("ID3D12Device::CreateCommittedResource failed", -EIO);

  return create_shared_handle(tf->device, (ID3D12DeviceChild *)tf->resource,
                              out_shared);
}

texlink_frame_t *texlink_d3d12_frame_wrap_resource(
    const texlink_d3d12_wrap_resource_desc_t *desc) {
  if (!desc || !desc->device || !desc->resource)
    return NULL;

  D3D12_RESOURCE_DESC rd;
  desc->resource->lpVtbl->GetDesc(desc->resource, &rd);
  HANDLE shared = desc->shared_handle;
  int owned = 0;
  if (!shared) {
    if (create_shared_handle(desc->device, (ID3D12DeviceChild *)desc->resource,
                             &shared) != 0)
      return NULL;
    owned = 1;
  }

  uint32_t format =
      desc->format ? desc->format : texlink_format_from_dxgi(rd.Format);
  uint32_t width = desc->width ? desc->width : (uint32_t)rd.Width;
  uint32_t height = desc->height ? desc->height : rd.Height;
  uint32_t stride = desc->stride ? desc->stride : default_stride(width, format);
  uint64_t size = desc->size ? desc->size : (uint64_t)stride * height;

  texlink_native_handle_t handle = {
      .handle_type = TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE,
      .owned = owned,
      .value.ptr = shared,
  };
  return texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
          .width = width,
          .height = height,
          .depth = 1,
          .format = format,
          .stride = stride,
          .size = size,
          .backend_type = TEXLINK_BACKEND_D3D12,
          .handle = handle,
      });
}

texlink_d3d12_texture_frame_t *texlink_d3d12_texture_frame_create(
    const texlink_d3d12_texture_frame_desc_t *desc) {
  if (!desc || !desc->device || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_d3d12_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;

  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  texlink_d3d12_texture_frame_desc_t normalized = *desc;
  normalized.format = format;

  HANDLE shared = NULL;
  if (set_device(&tf->device, desc->device) != 0 ||
      create_shared_resource(tf, &normalized, &shared) != 0)
    goto err;

  tf->frame = texlink_d3d12_frame_wrap_resource(
      &(texlink_d3d12_wrap_resource_desc_t){
          .device = tf->device,
          .resource = tf->resource,
          .shared_handle = shared,
          .width = desc->width,
          .height = desc->height,
          .format = format,
      });
  if (!tf->frame) {
    g_last_error = "texlink_d3d12_frame_wrap_resource failed";
    CloseHandle(shared);
    goto err;
  }
  CloseHandle(shared);

  tf->owns_frame = 1;
  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_d3d12_texture_frame_t *texlink_d3d12_texture_frame_import(
    const texlink_d3d12_import_desc_t *desc) {
  if (!desc || !desc->device || !desc->frame)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(
          desc->frame, TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE, &handle) != 0)
    return NULL;

  texlink_d3d12_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  tf->frame = desc->frame;

  HRESULT hr;
  if (set_device(&tf->device, desc->device) != 0)
    goto err;
  hr = tf->device->lpVtbl->OpenSharedHandle(
      tf->device, (HANDLE)handle.value.ptr, &IID_ID3D12Resource,
      (void **)&tf->resource);
  if (FAILED(hr) || !tf->resource) {
    fail("ID3D12Device::OpenSharedHandle(ID3D12Resource) failed", -EIO);
    goto err;
  }

  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_frame_t *texlink_d3d12_texture_frame_frame(
    texlink_d3d12_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

ID3D12Resource *texlink_d3d12_texture_frame_resource(
    texlink_d3d12_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->resource : NULL;
}

ID3D12Device *texlink_d3d12_texture_frame_device(
    texlink_d3d12_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->device : NULL;
}

void texlink_d3d12_texture_frame_destroy(
    texlink_d3d12_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  release_texture_frame(texture_frame);
  free(texture_frame);
}

texlink_d3d12_fence_frame_t *
texlink_d3d12_fence_frame_create(const texlink_d3d12_fence_frame_desc_t *desc) {
  if (!desc || !desc->device)
    return NULL;

  texlink_d3d12_fence_frame_t *ff = calloc(1, sizeof(*ff));
  if (!ff)
    return NULL;

  HANDLE shared = NULL;
  HRESULT hr;
  if (set_device(&ff->device, desc->device) != 0)
    goto err;
  hr = ff->device->lpVtbl->CreateFence(ff->device, desc->initial_value,
                                       desc->flags, &IID_ID3D12Fence,
                                       (void **)&ff->fence);
  if (FAILED(hr) || !ff->fence) {
    fail("ID3D12Device::CreateFence failed", -EIO);
    goto err;
  }
  if (create_shared_handle(ff->device, (ID3D12DeviceChild *)ff->fence,
                           &shared) != 0)
    goto err;

  texlink_native_handle_t handle = {
      .handle_type = TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
      .owned = 1,
      .value.ptr = shared,
  };
  ff->frame = texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_RAW,
          .width = 1,
          .height = 1,
          .depth = 1,
          .format = TEXLINK_FRAME_FORMAT_UNKNOWN,
          .stride = 0,
          .size = 1,
          .backend_type = TEXLINK_BACKEND_D3D12,
          .handle = handle,
      });
  if (!ff->frame) {
    g_last_error = "texlink_frame_create_from_native_handle failed";
    CloseHandle(shared);
    goto err;
  }

  ff->owns_frame = 1;
  return ff;

err:
  release_fence_frame(ff);
  free(ff);
  return NULL;
}

texlink_d3d12_fence_frame_t *
texlink_d3d12_fence_frame_import(const texlink_d3d12_fence_import_desc_t *desc) {
  if (!desc || !desc->device || !desc->frame)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(
          desc->frame, TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE, &handle) != 0)
    return NULL;

  texlink_d3d12_fence_frame_t *ff = calloc(1, sizeof(*ff));
  if (!ff)
    return NULL;
  ff->frame = desc->frame;

  HRESULT hr;
  if (set_device(&ff->device, desc->device) != 0)
    goto err;
  hr = ff->device->lpVtbl->OpenSharedHandle(
      ff->device, (HANDLE)handle.value.ptr, &IID_ID3D12Fence,
      (void **)&ff->fence);
  if (FAILED(hr) || !ff->fence) {
    fail("ID3D12Device::OpenSharedHandle(ID3D12Fence) failed", -EIO);
    goto err;
  }

  return ff;

err:
  release_fence_frame(ff);
  free(ff);
  return NULL;
}

texlink_frame_t *
texlink_d3d12_fence_frame_frame(texlink_d3d12_fence_frame_t *fence_frame) {
  return fence_frame ? fence_frame->frame : NULL;
}

ID3D12Fence *
texlink_d3d12_fence_frame_fence(texlink_d3d12_fence_frame_t *fence_frame) {
  return fence_frame ? fence_frame->fence : NULL;
}

void texlink_d3d12_fence_frame_destroy(
    texlink_d3d12_fence_frame_t *fence_frame) {
  if (!fence_frame)
    return;
  release_fence_frame(fence_frame);
  free(fence_frame);
}
