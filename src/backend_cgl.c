#include <texlink_cgl.h>

#include <OpenGL/CGLCurrent.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/gl3.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

struct texlink_cgl_texture_frame {
  texlink_frame_t *frame;
  GLuint texture;
  GLenum target;
  int owns_frame;
};

static const char *g_last_error = "no error";

static int fail(const char *message, int code) {
  g_last_error = message;
  return code;
}

const char *texlink_cgl_last_error_string(void) {
  return g_last_error;
}

static void gl_format(uint32_t format, GLenum *internal_format, GLenum *fmt,
                      GLenum *type) {
  switch (format) {
  case TEXLINK_FRAME_FORMAT_R8:
    *internal_format = GL_R8;
    *fmt = GL_RED;
    *type = GL_UNSIGNED_BYTE;
    break;
  case TEXLINK_FRAME_FORMAT_ABGR8888:
  case TEXLINK_FRAME_FORMAT_XBGR8888:
    *internal_format = GL_RGBA8;
    *fmt = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
    break;
  case TEXLINK_FRAME_FORMAT_ARGB8888:
  case TEXLINK_FRAME_FORMAT_XRGB8888:
  default:
    *internal_format = GL_RGBA8;
    *fmt = GL_BGRA;
    *type = GL_UNSIGNED_INT_8_8_8_8_REV;
    break;
  }
}

static int bind_frame_to_texture(texlink_cgl_texture_frame_t *tf) {
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

  GLenum internal_format, format, type;
  gl_format(meta.format, &internal_format, &format, &type);
  CGLContextObj ctx = CGLGetCurrentContext();
  if (!ctx) {
    CFRelease(surface);
    return fail("no current CGL context", -EINVAL);
  }

  if (!tf->texture)
    glGenTextures(1, &tf->texture);
  if (!tf->texture) {
    CFRelease(surface);
    return fail("glGenTextures failed", -EIO);
  }

  glBindTexture(tf->target, tf->texture);
  glTexParameteri(tf->target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(tf->target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(tf->target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(tf->target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  CGLError err =
      CGLTexImageIOSurface2D(ctx, tf->target, internal_format, meta.width,
                             meta.height, format, type, surface, 0);
  CFRelease(surface);
  return err == kCGLNoError ? 0 : fail("CGLTexImageIOSurface2D failed", -EIO);
}

static void release_texture_frame(texlink_cgl_texture_frame_t *tf) {
  if (!tf)
    return;
  if (tf->texture)
    glDeleteTextures(1, &tf->texture);
  if (tf->owns_frame)
    texlink_frame_destroy(tf->frame);
}

texlink_cgl_texture_frame_t *texlink_cgl_texture_frame_create(
    const texlink_cgl_texture_frame_desc_t *desc) {
  if (!desc || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_cgl_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  uint32_t format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888;
  tf->target = desc->target ? desc->target : GL_TEXTURE_RECTANGLE;
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
  if (bind_frame_to_texture(tf) != 0)
    goto err;
  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_cgl_texture_frame_t *texlink_cgl_texture_frame_import(
    const texlink_cgl_import_desc_t *desc) {
  if (!desc || !desc->frame)
    return NULL;

  texlink_cgl_texture_frame_t *tf = calloc(1, sizeof(*tf));
  if (!tf)
    return NULL;
  tf->frame = desc->frame;
  tf->target = desc->target ? desc->target : GL_TEXTURE_RECTANGLE;

  if (bind_frame_to_texture(tf) != 0)
    goto err;
  return tf;

err:
  release_texture_frame(tf);
  free(tf);
  return NULL;
}

texlink_frame_t *texlink_cgl_texture_frame_frame(
    texlink_cgl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

unsigned int texlink_cgl_texture_frame_texture(
    texlink_cgl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->texture : 0;
}

void texlink_cgl_texture_frame_destroy(
    texlink_cgl_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  release_texture_frame(texture_frame);
  free(texture_frame);
}
