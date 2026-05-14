#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <texlink_d3d11.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct texlink_d3d11_texture_frame {
  texlink_frame_t *frame;
  ID3D11Device *device;
  ID3D11DeviceContext *context;
  ID3D11Texture2D *texture;
  int owns_device;
  int owns_frame;
};

static const char *g_last_error = "no error";

static int fail(const char *message, int code) {
  g_last_error = message;
  return code;
}

const char *texlink_d3d11_last_error_string(void) {
  return g_last_error;
}

DXGI_FORMAT texlink_d3d11_format(uint32_t format) {
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

static void release_texture_frame(texlink_d3d11_texture_frame_t *tf) {
  if (!tf)
    return;
  if (tf->owns_frame)
    texlink_frame_destroy(tf->frame);
  if (tf->texture)
    tf->texture->lpVtbl->Release(tf->texture);
  if (tf->context)
    tf->context->lpVtbl->Release(tf->context);
  if (tf->device)
    tf->device->lpVtbl->Release(tf->device);
}

static int set_device(texlink_d3d11_texture_frame_t *tf,
                      ID3D11Device *device) {
  if (device) {
    tf->device = device;
    tf->device->lpVtbl->AddRef(tf->device);
    tf->device->lpVtbl->GetImmediateContext(tf->device, &tf->context);
    return 0;
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected = 0;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                 levels,
                                 (UINT)(sizeof(levels) / sizeof(levels[0])),
                                 D3D11_SDK_VERSION, &tf->device, &selected,
                                 &tf->context);
  tf->owns_device = SUCCEEDED(hr);
  return SUCCEEDED(hr) ? 0 : fail("D3D11CreateDevice failed", -EIO);
}

static int shared_handle_from_texture(ID3D11Texture2D *texture,
                                      HANDLE *out_shared,
                                      texlink_native_handle_type_t *out_type,
                                      uint32_t *out_flags) {
  D3D11_TEXTURE2D_DESC td;
  texture->lpVtbl->GetDesc(texture, &td);

  if (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
    IDXGIResource1 *resource = NULL;
    HRESULT hr = texture->lpVtbl->QueryInterface(
        texture, &IID_IDXGIResource1, (void **)&resource);
    if (FAILED(hr) || !resource)
      return fail("ID3D11Texture2D::QueryInterface(IDXGIResource1) failed",
                  -EIO);

    hr = resource->lpVtbl->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL,
                                              out_shared);
    resource->lpVtbl->Release(resource);
    if (FAILED(hr) || !*out_shared)
      return fail("IDXGIResource1::CreateSharedHandle failed", -EIO);
    if (out_type)
      *out_type = TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE;
    if (out_flags)
      *out_flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED;
    return 0;
  }

  IDXGIResource *resource = NULL;
  HRESULT hr = texture->lpVtbl->QueryInterface(
      texture, &IID_IDXGIResource, (void **)&resource);
  if (FAILED(hr) || !resource)
    return fail("ID3D11Texture2D::QueryInterface(IDXGIResource) failed", -EIO);

  hr = resource->lpVtbl->GetSharedHandle(resource, out_shared);
  resource->lpVtbl->Release(resource);
  if (FAILED(hr) || !*out_shared)
    return fail("IDXGIResource::GetSharedHandle failed", -EIO);
  if (out_type)
    *out_type = TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE;
  if (out_flags)
    *out_flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  return 0;
}

static int create_shared_texture(texlink_d3d11_texture_frame_t *tf,
                                 uint32_t width, uint32_t height,
                                 uint32_t format, UINT bind_flags,
                                 UINT misc_flags, HANDLE *out_shared,
                                 texlink_native_handle_type_t *out_type,
                                 uint32_t *out_flags) {
  DXGI_FORMAT dxgi_format = texlink_d3d11_format(format);
  if (dxgi_format == DXGI_FORMAT_UNKNOWN)
    return fail("unsupported D3D11 texture format", -EINVAL);

  D3D11_TEXTURE2D_DESC td;
  memset(&td, 0, sizeof(td));
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = dxgi_format;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = bind_flags ? bind_flags
                            : (D3D11_BIND_RENDER_TARGET |
                               D3D11_BIND_SHADER_RESOURCE);
  td.MiscFlags = misc_flags ? misc_flags : D3D11_RESOURCE_MISC_SHARED;

  HRESULT hr =
      tf->device->lpVtbl->CreateTexture2D(tf->device, &td, NULL, &tf->texture);
  if (FAILED(hr) && (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) &&
      !(td.MiscFlags & D3D11_RESOURCE_MISC_SHARED)) {
    td.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
    hr =
        tf->device->lpVtbl->CreateTexture2D(tf->device, &td, NULL, &tf->texture);
  }
  if (FAILED(hr) || !tf->texture)
    return fail("ID3D11Device::CreateTexture2D failed", -EIO);

  return shared_handle_from_texture(tf->texture, out_shared, out_type,
                                    out_flags);
}

static int open_shared_texture(texlink_d3d11_texture_frame_t *tf,
                               HANDLE shared_handle,
                               texlink_native_handle_type_t handle_type) {
  HRESULT hr;
  if (handle_type == TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE) {
    ID3D11Device1 *device1 = NULL;
    hr = tf->device->lpVtbl->QueryInterface(tf->device, &IID_ID3D11Device1,
                                            (void **)&device1);
    if (FAILED(hr) || !device1)
      return fail("ID3D11Device::QueryInterface(ID3D11Device1) failed", -EIO);
    hr = device1->lpVtbl->OpenSharedResource1(
        device1, shared_handle, &IID_ID3D11Texture2D, (void **)&tf->texture);
    device1->lpVtbl->Release(device1);
  } else {
    hr = tf->device->lpVtbl->OpenSharedResource(
        tf->device, shared_handle, &IID_ID3D11Texture2D, (void **)&tf->texture);
  }
  return SUCCEEDED(hr) && tf->texture
             ? 0
             : fail("ID3D11Device::OpenSharedResource failed", -EIO);
}

texlink_frame_t *texlink_d3d11_frame_wrap_texture(
    const texlink_d3d11_wrap_texture_desc_t *desc) {
  if (!desc || !desc->texture)
    return NULL;

  D3D11_TEXTURE2D_DESC td;
  desc->texture->lpVtbl->GetDesc(desc->texture, &td);

  HANDLE shared = desc->shared_handle;
  texlink_native_handle_type_t handle_type = TEXLINK_NATIVE_HANDLE_UNKNOWN;
  uint32_t handle_flags =
      desc->flags ? desc->flags : TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  if (!shared &&
      shared_handle_from_texture(desc->texture, &shared, &handle_type,
                                 &handle_flags) != 0)
    return NULL;
  if (handle_type == TEXLINK_NATIVE_HANDLE_UNKNOWN) {
    handle_type = (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)
                      ? TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE
                      : TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE;
  }

  uint32_t format =
      desc->format ? desc->format : texlink_format_from_dxgi(td.Format);
  uint32_t stride =
      desc->stride
          ? desc->stride
          : default_stride(desc->width ? desc->width : td.Width, format);
  uint32_t width = desc->width ? desc->width : td.Width;
  uint32_t height = desc->height ? desc->height : td.Height;
  uint64_t size = desc->size ? desc->size : (uint64_t)stride * height;

  texlink_native_handle_t handle = {
      .version = 1,
      .type = handle_type,
      .flags = handle_flags,
      .value.ptr = shared,
  };
  return texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .version = 1,
          .type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
          .width = width,
          .height = height,
          .depth = 1,
          .format = format,
          .stride = stride,
          .size = size,
          .backend = TEXLINK_BACKEND_D3D11,
          .handle = handle,
      });
}

texlink_d3d11_texture_frame_t *texlink_d3d11_texture_frame_create(
    const texlink_d3d11_texture_frame_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_d3d11_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;

  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  HANDLE shared = NULL;
  texlink_native_handle_type_t handle_type = TEXLINK_NATIVE_HANDLE_UNKNOWN;
  uint32_t handle_flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED;
  if (set_device(tf, desc->device) != 0 ||
      create_shared_texture(tf, desc->width, desc->height, format,
                            desc->bind_flags, desc->misc_flags, &shared,
                            &handle_type, &handle_flags) != 0)
    goto err;

  tf->frame = texlink_d3d11_frame_wrap_texture(
      &(texlink_d3d11_wrap_texture_desc_t){
          .version = 1,
          .texture = tf->texture,
          .shared_handle = shared,
          .width = desc->width,
          .height = desc->height,
          .format = format,
          .flags = handle_flags,
      });
  if (!tf->frame) {
    g_last_error = "texlink_d3d11_frame_wrap_texture failed";
    goto err;
  }

  tf->owns_frame = 1;
  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_d3d11_texture_frame_t *texlink_d3d11_texture_frame_import(
    const texlink_d3d11_import_desc_t *desc) {
  if (!desc || !desc->frame)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(
          desc->frame, TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE, &handle) !=
      0) {
    if (texlink_frame_get_native_handle(
            desc->frame, TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE, &handle) !=
        0)
      return NULL;
  }

  texlink_d3d11_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  tf->frame = desc->frame;

  if (set_device(tf, desc->device) != 0 ||
      open_shared_texture(tf, (HANDLE)handle.value.ptr, handle.type) != 0)
    goto err;

  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_frame_t *texlink_d3d11_texture_frame_frame(
    texlink_d3d11_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

ID3D11Texture2D *texlink_d3d11_texture_frame_texture(
    texlink_d3d11_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->texture : NULL;
}

ID3D11Device *texlink_d3d11_texture_frame_device(
    texlink_d3d11_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->device : NULL;
}

void texlink_d3d11_texture_frame_destroy(
    texlink_d3d11_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  release_texture_frame(texture_frame);
  free(texture_frame);
}
