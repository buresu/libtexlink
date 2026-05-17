#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <GL/gl.h>
#include <texlink_wgl.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef WGL_ACCESS_READ_WRITE_NV
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#endif

typedef HANDLE(WINAPI *PFNWGLDXOPENDEVICENVPROC)(void *dx_device);
typedef BOOL(WINAPI *PFNWGLDXCLOSEDEVICENVPROC)(HANDLE dx_device);
typedef HANDLE(WINAPI *PFNWGLDXREGISTEROBJECTNVPROC)(
    HANDLE dx_device, void *dx_object, GLuint name, GLenum type, GLenum access);
typedef BOOL(WINAPI *PFNWGLDXUNREGISTEROBJECTNVPROC)(HANDLE dx_device,
                                                     HANDLE dx_object);
typedef BOOL(WINAPI *PFNWGLDXLOCKOBJECTSNVPROC)(HANDLE dx_device, GLint count,
                                                HANDLE *dx_objects);
typedef BOOL(WINAPI *PFNWGLDXUNLOCKOBJECTSNVPROC)(HANDLE dx_device, GLint count,
                                                  HANDLE *dx_objects);

typedef HGLRC(WINAPI *PFNWGLGETCURRENTCONTEXTPROC)(void);
typedef PROC(WINAPI *PFNWGLGETPROCADDRESSPROC)(LPCSTR name);

__declspec(dllimport) HGLRC WINAPI wglGetCurrentContext(void);
__declspec(dllimport) PROC WINAPI wglGetProcAddress(LPCSTR name);

typedef struct {
  PFNWGLDXOPENDEVICENVPROC open_device;
  PFNWGLDXCLOSEDEVICENVPROC close_device;
  PFNWGLDXREGISTEROBJECTNVPROC register_object;
  PFNWGLDXUNREGISTEROBJECTNVPROC unregister_object;
  PFNWGLDXLOCKOBJECTSNVPROC lock_objects;
  PFNWGLDXUNLOCKOBJECTSNVPROC unlock_objects;
} texlink_wgl_dx_interop_t;

struct texlink_wgl_texture_frame {
  texlink_frame_t *frame;
  GLuint texture;
  ID3D11Device *device;
  ID3D11DeviceContext *context;
  ID3D11Texture2D *texture2d;
  HANDLE dx_device;
  HANDLE dx_object;
  int owns_frame;
  int locked;
};

static const char *g_last_error = "no error";

static int fail(const char *message, int code) {
  g_last_error = message;
  return code;
}

const char *texlink_wgl_last_error_string(void) {
  return g_last_error;
}

static int load_interop(texlink_wgl_dx_interop_t *out) {
  memset(out, 0, sizeof(*out));
  out->open_device =
      (PFNWGLDXOPENDEVICENVPROC)wglGetProcAddress("wglDXOpenDeviceNV");
  out->close_device =
      (PFNWGLDXCLOSEDEVICENVPROC)wglGetProcAddress("wglDXCloseDeviceNV");
  out->register_object = (PFNWGLDXREGISTEROBJECTNVPROC)wglGetProcAddress(
      "wglDXRegisterObjectNV");
  out->unregister_object = (PFNWGLDXUNREGISTEROBJECTNVPROC)wglGetProcAddress(
      "wglDXUnregisterObjectNV");
  out->lock_objects =
      (PFNWGLDXLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXLockObjectsNV");
  out->unlock_objects =
      (PFNWGLDXUNLOCKOBJECTSNVPROC)wglGetProcAddress("wglDXUnlockObjectsNV");
  if (!(out->open_device && out->close_device && out->register_object &&
        out->unregister_object && out->lock_objects && out->unlock_objects)) {
    g_last_error = "WGL_NV_DX_interop is not available";
    return 0;
  }
  return 1;
}

static DXGI_FORMAT d3d_format(uint32_t format) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return DXGI_FORMAT_R8_UNORM;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
  default:
    return DXGI_FORMAT_B8G8R8A8_UNORM;
  }
}

static uint32_t default_stride(uint32_t width, uint32_t format) {
  return width * (format == TEXLINK_FRAME_FORMAT_R8 ? 1u : 4u);
}

static void release_frame(texlink_wgl_texture_frame_t *tf) {
  if (!tf)
    return;

  texlink_wgl_dx_interop_t interop;
  int have_interop = load_interop(&interop);
  if (have_interop && tf->dx_object) {
    if (tf->locked)
      interop.unlock_objects(tf->dx_device, 1, &tf->dx_object);
    interop.unregister_object(tf->dx_device, tf->dx_object);
  }
  if (have_interop && tf->dx_device)
    interop.close_device(tf->dx_device);
  if (tf->texture)
    glDeleteTextures(1, &tf->texture);
  if (tf->texture2d)
    tf->texture2d->lpVtbl->Release(tf->texture2d);
  if (tf->context)
    tf->context->lpVtbl->Release(tf->context);
  if (tf->device)
    tf->device->lpVtbl->Release(tf->device);
  if (tf->owns_frame)
    texlink_frame_destroy(tf->frame);
}

static int create_device(texlink_wgl_texture_frame_t *tf) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected = 0;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                 levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
                                 D3D11_SDK_VERSION, &tf->device, &selected,
                                 &tf->context);
  return SUCCEEDED(hr) ? 0 : fail("D3D11CreateDevice failed", -EIO);
}

static int create_shared_texture(texlink_wgl_texture_frame_t *tf,
                                 uint32_t width, uint32_t height,
                                 uint32_t format, HANDLE *out_shared,
                                 texlink_native_handle_type_t *out_type,
                                 int *out_owned) {
  D3D11_TEXTURE2D_DESC td;
  memset(&td, 0, sizeof(td));
  td.Width = width;
  td.Height = height;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = d3d_format(format);
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

  HRESULT hr =
      tf->device->lpVtbl->CreateTexture2D(tf->device, &td, NULL, &tf->texture2d);
  if (FAILED(hr)) {
    td.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
    hr = tf->device->lpVtbl->CreateTexture2D(tf->device, &td, NULL,
                                             &tf->texture2d);
  }
  if (FAILED(hr))
    return fail("ID3D11Device::CreateTexture2D failed", -EIO);

  tf->texture2d->lpVtbl->GetDesc(tf->texture2d, &td);
  if (td.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) {
    IDXGIResource1 *resource = NULL;
    hr = tf->texture2d->lpVtbl->QueryInterface(
        tf->texture2d, &IID_IDXGIResource1, (void **)&resource);
    if (FAILED(hr) || !resource)
      return fail("ID3D11Texture2D::QueryInterface(IDXGIResource1) failed",
                  -EIO);
    hr = resource->lpVtbl->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL,
                                              out_shared);
    resource->lpVtbl->Release(resource);
    if (FAILED(hr) || !*out_shared)
      return fail("IDXGIResource1::CreateSharedHandle failed", -EIO);
    *out_type = TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE;
    *out_owned = 1;
    return 0;
  }

  IDXGIResource *resource = NULL;
  hr = tf->texture2d->lpVtbl->QueryInterface(
      tf->texture2d, &IID_IDXGIResource, (void **)&resource);
  if (FAILED(hr) || !resource)
    return fail("ID3D11Texture2D::QueryInterface(IDXGIResource) failed", -EIO);

  hr = resource->lpVtbl->GetSharedHandle(resource, out_shared);
  resource->lpVtbl->Release(resource);
  if (FAILED(hr) || !*out_shared)
    return fail("IDXGIResource::GetSharedHandle failed", -EIO);
  *out_type = TEXLINK_NATIVE_HANDLE_D3D11_SHARED_HANDLE;
  *out_owned = 0;
  return 0;
}

static int open_shared_texture(texlink_wgl_texture_frame_t *tf,
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
        device1, shared_handle, &IID_ID3D11Texture2D, (void **)&tf->texture2d);
    device1->lpVtbl->Release(device1);
  } else {
    hr = tf->device->lpVtbl->OpenSharedResource(
        tf->device, shared_handle, &IID_ID3D11Texture2D,
        (void **)&tf->texture2d);
  }
  return SUCCEEDED(hr) && tf->texture2d
             ? 0
             : fail("ID3D11Device::OpenSharedResource failed", -EIO);
}

static int register_gl_texture(texlink_wgl_texture_frame_t *tf) {
  texlink_wgl_dx_interop_t interop;
  if (!load_interop(&interop))
    return -ENOTSUP;
  if (!wglGetCurrentContext())
    return fail("no current WGL context", -EINVAL);

  glGenTextures(1, &tf->texture);
  if (!tf->texture)
    return fail("glGenTextures failed", -EIO);
  glBindTexture(GL_TEXTURE_2D, tf->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

  tf->dx_device = interop.open_device(tf->device);
  if (!tf->dx_device)
    return fail("wglDXOpenDeviceNV failed", -EIO);

  tf->dx_object =
      interop.register_object(tf->dx_device, tf->texture2d, tf->texture,
                              GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
  return tf->dx_object ? 0 : fail("wglDXRegisterObjectNV failed", -EIO);
}

texlink_wgl_texture_frame_t *
texlink_wgl_texture_frame_create(const texlink_wgl_texture_frame_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_wgl_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;

  HANDLE shared = NULL;
  texlink_native_handle_type_t handle_type = TEXLINK_NATIVE_HANDLE_UNKNOWN;
  int handle_owned = 0;
  if (create_device(tf) != 0 ||
      create_shared_texture(tf, desc->width, desc->height, format, &shared,
                            &handle_type, &handle_owned) != 0 ||
      register_gl_texture(tf) != 0)
    goto err;

  texlink_native_handle_t handle = {
      .handle_type = handle_type,
      .owned = handle_owned,
      .value.ptr = shared,
  };
  tf->frame = texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
          .width = desc->width,
          .height = desc->height,
          .depth = 1,
          .format = format,
          .stride = default_stride(desc->width, format),
          .size = (uint64_t)default_stride(desc->width, format) * desc->height,
          .backend_type = TEXLINK_BACKEND_WGL,
          .handle = handle,
      });
  if (!tf->frame) {
    g_last_error = "texlink_frame_create_from_native_handle failed";
    goto err;
  }
  tf->owns_frame = 1;
  return tf;

err:
  release_frame(tf);
  free(tf);
  return NULL;
}

texlink_wgl_texture_frame_t *
texlink_wgl_texture_frame_import(const texlink_wgl_import_desc_t *desc) {
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

  texlink_wgl_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  tf->frame = desc->frame;

  if (create_device(tf) != 0 ||
      open_shared_texture(tf, (HANDLE)handle.value.ptr, handle.handle_type) != 0 ||
      register_gl_texture(tf) != 0)
    goto err;

  return tf;

err:
  release_frame(tf);
  free(tf);
  return NULL;
}

texlink_frame_t *
texlink_wgl_texture_frame_frame(texlink_wgl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

unsigned int
texlink_wgl_texture_frame_texture(texlink_wgl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->texture : 0;
}

int texlink_wgl_texture_frame_lock(texlink_wgl_texture_frame_t *texture_frame) {
  if (!texture_frame || !texture_frame->dx_device || !texture_frame->dx_object)
    return -EINVAL;
  if (texture_frame->locked)
    return 0;
  texlink_wgl_dx_interop_t interop;
  if (!load_interop(&interop))
    return -ENOTSUP;
  if (!interop.lock_objects(texture_frame->dx_device, 1,
                            &texture_frame->dx_object))
    return -EIO;
  texture_frame->locked = 1;
  return 0;
}

int texlink_wgl_texture_frame_unlock(
    texlink_wgl_texture_frame_t *texture_frame) {
  if (!texture_frame || !texture_frame->dx_device || !texture_frame->dx_object)
    return -EINVAL;
  if (!texture_frame->locked)
    return 0;
  texlink_wgl_dx_interop_t interop;
  if (!load_interop(&interop))
    return -ENOTSUP;
  if (!interop.unlock_objects(texture_frame->dx_device, 1,
                              &texture_frame->dx_object))
    return -EIO;
  if (texture_frame->context)
    texture_frame->context->lpVtbl->Flush(texture_frame->context);
  texture_frame->locked = 0;
  return 0;
}

void texlink_wgl_texture_frame_destroy(
    texlink_wgl_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  release_frame(texture_frame);
  free(texture_frame);
}
