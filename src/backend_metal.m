#include <texlink_metal.h>

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

struct texlink_metal_texture_frame {
  texlink_frame_t *frame;
  id<MTLDevice> device;
  id<MTLTexture> texture;
  int owns_frame;
};

static const char *g_last_error = "no error";

static int fail(const char *message, int code) {
  g_last_error = message;
  return code;
}

const char *texlink_metal_last_error_string(void) {
  return g_last_error;
}

static MTLPixelFormat metal_format(uint32_t format) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    return MTLPixelFormatR8Unorm;
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    return MTLPixelFormatRGBA8Unorm;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
  default:
    return MTLPixelFormatBGRA8Unorm;
  }
}

static uint32_t texlink_format_from_metal(MTLPixelFormat format) {
  switch (format) {
  case MTLPixelFormatR8Unorm:
    return TEXLINK_FRAME_FORMAT_R8;
  case MTLPixelFormatRGBA8Unorm:
  case MTLPixelFormatRGBA8Unorm_sRGB:
    return TEXLINK_FRAME_FORMAT_ABGR8888;
  case MTLPixelFormatBGRA8Unorm:
  case MTLPixelFormatBGRA8Unorm_sRGB:
  default:
    return TEXLINK_FRAME_FORMAT_ARGB8888;
  }
}

static uint32_t default_stride(uint32_t width, uint32_t format) {
  return width * (format == TEXLINK_FRAME_FORMAT_R8 ? 1u : 4u);
}

static int set_device(texlink_metal_texture_frame_t *tf, void *device) {
  tf->device = device ? [(id<MTLDevice>)device retain]
                      : MTLCreateSystemDefaultDevice();
  return tf->device ? 0 : fail("MTLCreateSystemDefaultDevice failed", -EIO);
}

static int create_texture(texlink_metal_texture_frame_t *tf, uint32_t usage) {
  texlink_meta_t meta = texlink_frame_meta(tf->frame);
  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(tf->frame,
                                      TEXLINK_NATIVE_HANDLE_IOSURFACE_ID,
                                      &handle) != 0)
    return fail("frame does not expose an IOSurface ID", -EINVAL);

  IOSurfaceRef surface =
      IOSurfaceLookup((IOSurfaceID)(uintptr_t)handle.value.ptr);
  if (!surface)
    return fail("IOSurfaceLookup failed", -EIO);

  MTLTextureDescriptor *td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:metal_format(meta.format)
                                   width:meta.width
                                  height:meta.height
                               mipmapped:NO];
  td.usage = usage ? (MTLTextureUsage)usage
                   : (MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite |
                      MTLTextureUsageRenderTarget);
  td.storageMode = MTLStorageModeShared;

  tf->texture = [tf->device newTextureWithDescriptor:td
                                           iosurface:surface
                                               plane:0];
  CFRelease(surface);
  return tf->texture ? 0 : fail("newTextureWithDescriptor:iosurface failed",
                                -EIO);
}

static void release_texture_frame(texlink_metal_texture_frame_t *tf) {
  if (!tf)
    return;
  if (tf->texture)
    [tf->texture release];
  if (tf->device)
    [tf->device release];
  if (tf->owns_frame)
    texlink_frame_destroy(tf->frame);
}

texlink_metal_texture_frame_t *texlink_metal_texture_frame_create(
    const texlink_metal_texture_frame_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_metal_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;

  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  tf->frame = texlink_frame_create(&(texlink_frame_desc_t){
      .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .format = format,
  });
  if (!tf->frame) {
    g_last_error = "texlink_frame_create failed";
    goto err;
  }
  tf->owns_frame = 1;
  if (set_device(tf, desc->device) != 0 || create_texture(tf, desc->usage) != 0)
    goto err;

  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_metal_texture_frame_t *texlink_metal_texture_frame_import(
    const texlink_metal_import_desc_t *desc) {
  if (!desc || !desc->frame)
    return NULL;

  texlink_metal_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  tf->frame = desc->frame;

  if (set_device(tf, desc->device) != 0 || create_texture(tf, desc->usage) != 0)
    goto err;

  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_frame_t *texlink_metal_frame_wrap_texture(
    const texlink_metal_wrap_texture_desc_t *desc) {
  if (!desc || !desc->texture)
    return NULL;

  id<MTLTexture> texture = (id<MTLTexture>)desc->texture;
  IOSurfaceRef surface = texture.iosurface;
  if (!surface)
    return NULL;

  uint32_t format = desc->format ? desc->format
                                 : texlink_format_from_metal(texture.pixelFormat);
  uint32_t width = (uint32_t)texture.width;
  uint32_t height = (uint32_t)texture.height;
  IOSurfaceID surface_id = IOSurfaceGetID(surface);
  texlink_native_handle_t handle = {
      .handle_type = TEXLINK_NATIVE_HANDLE_IOSURFACE_ID,
      .owned = 0,
      .value.ptr = (void *)(uintptr_t)surface_id,
  };
  return texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
          .width = width,
          .height = height,
          .depth = 1,
          .format = format,
          .stride = default_stride(width, format),
          .size = (uint64_t)default_stride(width, format) * height,
          .modifier = surface_id,
          .backend_type = TEXLINK_BACKEND_METAL,
          .handle = handle,
      });
}

texlink_frame_t *texlink_metal_texture_frame_frame(
    texlink_metal_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

void *texlink_metal_texture_frame_texture(
    texlink_metal_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->texture : NULL;
}

void *texlink_metal_texture_frame_device(
    texlink_metal_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->device : NULL;
}

void texlink_metal_texture_frame_destroy(
    texlink_metal_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  release_texture_frame(texture_frame);
  free(texture_frame);
}
