#define GL_GLEXT_PROTOTYPES
#include <GL/glew.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <texlink_egl.h>

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC TargetTex2D_fn;

struct texlink_egl_image {
  EGLDisplay display;
  EGLImage image;
};

struct texlink_egl_texture_frame {
  texlink_frame_t *frame;
  texlink_egl_image_t *image;
  unsigned int texture;
};

static EGLImage import_dma_buf(EGLDisplay display, int fd,
                               const texlink_meta_t *m) {
  EGLAttrib attrs[] = {
      EGL_WIDTH,
      (EGLAttrib)m->width,
      EGL_HEIGHT,
      (EGLAttrib)m->height,
      EGL_LINUX_DRM_FOURCC_EXT,
      (EGLAttrib)m->format,
      EGL_DMA_BUF_PLANE0_FD_EXT,
      (EGLAttrib)fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      (EGLAttrib)m->stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      (EGLAttrib)(m->modifier & 0xFFFFFFFFu),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      (EGLAttrib)(m->modifier >> 32),
      EGL_NONE,
  };
  return eglCreateImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                        attrs);
}

static int bind_egl_image_to_texture(EGLImage image, unsigned int texture) {
  TargetTex2D_fn target_texture =
      (TargetTex2D_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if (!target_texture)
    return -ENOTSUP;

  glBindTexture(GL_TEXTURE_2D, texture);
  target_texture(GL_TEXTURE_2D, image);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  return glGetError() == GL_NO_ERROR ? 0 : -EIO;
}

texlink_egl_image_t *
texlink_egl_image_import(const texlink_egl_import_desc_t *desc) {
  if (!desc || !desc->display || !desc->frame)
    return NULL;

  texlink_native_handle_t handle;
  if (texlink_frame_get_native_handle(
          desc->frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD, &handle) != 0)
    return NULL;

  texlink_meta_t meta = texlink_frame_meta(desc->frame);
  EGLImage image = import_dma_buf(desc->display, handle.value.fd, &meta);
  if (image == EGL_NO_IMAGE)
    return NULL;

  texlink_egl_image_t *egl_image = calloc(1, sizeof(*egl_image));
  if (!egl_image) {
    eglDestroyImage(desc->display, image);
    return NULL;
  }
  egl_image->display = desc->display;
  egl_image->image = image;
  return egl_image;
}

EGLImage texlink_egl_image_handle(texlink_egl_image_t *image) {
  return image ? image->image : EGL_NO_IMAGE;
}

int texlink_egl_image_bind_texture_2d(texlink_egl_image_t *image,
                                      unsigned int texture) {
  if (!image || image->image == EGL_NO_IMAGE || texture == 0)
    return -EINVAL;
  return bind_egl_image_to_texture(image->image, texture);
}

void texlink_egl_image_destroy(texlink_egl_image_t *image) {
  if (!image)
    return;
  if (image->image != EGL_NO_IMAGE)
    eglDestroyImage(image->display, image->image);
  free(image);
}

texlink_egl_texture_frame_t *
texlink_egl_texture_frame_create(const texlink_egl_texture_frame_desc_t *desc) {
  if (!desc || !desc->display || desc->width == 0 || desc->height == 0)
    return NULL;

  texlink_egl_texture_frame_t *texture_frame =
      calloc(1, sizeof(*texture_frame));
  if (!texture_frame)
    return NULL;

  texture_frame->frame = texlink_frame_create(&(texlink_frame_desc_t){
      .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .format = desc->format ? desc->format : TEXLINK_FRAME_FORMAT_ARGB8888,
  });
  if (!texture_frame->frame)
    goto err;

  texture_frame->image = texlink_egl_image_import(&(texlink_egl_import_desc_t){
      .display = desc->display,
      .frame = texture_frame->frame,
  });
  if (!texture_frame->image)
    goto err;

  glGenTextures(1, &texture_frame->texture);
  if (texture_frame->texture == 0)
    goto err;
  if (texlink_egl_image_bind_texture_2d(texture_frame->image,
                                        texture_frame->texture) != 0)
    goto err;

  return texture_frame;

err:
  texlink_egl_texture_frame_destroy(texture_frame);
  return NULL;
}

texlink_frame_t *
texlink_egl_texture_frame_frame(texlink_egl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->frame : NULL;
}

unsigned int
texlink_egl_texture_frame_texture(texlink_egl_texture_frame_t *texture_frame) {
  return texture_frame ? texture_frame->texture : 0;
}

void texlink_egl_texture_frame_destroy(
    texlink_egl_texture_frame_t *texture_frame) {
  if (!texture_frame)
    return;
  if (texture_frame->texture != 0)
    glDeleteTextures(1, &texture_frame->texture);
  texlink_egl_image_destroy(texture_frame->image);
  texlink_frame_destroy(texture_frame->frame);
  free(texture_frame);
}

texlink_frame_t *
texlink_egl_frame_wrap_texture_2d(const texlink_egl_wrap_texture_desc_t *desc) {
  if (!desc || !desc->display || desc->texture == 0 || desc->width == 0 ||
      desc->height == 0)
    return NULL;

  PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query =
      (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress(
          "eglExportDMABUFImageQueryMESA");
  PFNEGLEXPORTDMABUFIMAGEMESAPROC export_image =
      (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress(
          "eglExportDMABUFImageMESA");
  if (!query || !export_image)
    return NULL;

  EGLAttrib attrs[] = {EGL_GL_TEXTURE_LEVEL_KHR, 0, EGL_NONE};
  EGLImage image =
      eglCreateImage(desc->display, desc->context, EGL_GL_TEXTURE_2D_KHR,
                     (EGLClientBuffer)(uintptr_t)desc->texture, attrs);
  if (image == EGL_NO_IMAGE)
    return NULL;

  int fourcc = 0;
  int num_planes = 0;
  EGLuint64KHR modifiers[4] = {0};
  if (!query(desc->display, image, &fourcc, &num_planes, modifiers) ||
      num_planes != 1) {
    eglDestroyImage(desc->display, image);
    return NULL;
  }

  int fds[4] = {-1, -1, -1, -1};
  EGLint strides[4] = {0};
  EGLint offsets[4] = {0};
  if (!export_image(desc->display, image, fds, strides, offsets) ||
      fds[0] < 0 || offsets[0] != 0) {
    for (int i = 0; i < 4; i++) {
      if (fds[i] >= 0)
        close(fds[i]);
    }
    eglDestroyImage(desc->display, image);
    return NULL;
  }
  eglDestroyImage(desc->display, image);

  texlink_native_handle_t handle = {
      .handle_type = TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
      .flags = TEXLINK_NATIVE_HANDLE_FLAG_OWNED,
      .value.fd = fds[0],
  };
  return texlink_frame_create_from_native_handle(&(texlink_frame_native_desc_t){
      .frame_type = desc->target ? (texlink_frame_type_t)desc->target
                         : TEXLINK_FRAME_TYPE_TEXTURE_2D,
      .width = desc->width,
      .height = desc->height,
      .depth = 1,
      .format = desc->format ? desc->format : (uint32_t)fourcc,
      .stride = (uint32_t)strides[0],
      .modifier = modifiers[0],
      .size = (uint64_t)strides[0] * desc->height,
      .backend_type = TEXLINK_BACKEND_EGL,
      .handle = handle,
  });
}
