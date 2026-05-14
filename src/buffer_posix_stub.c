#include "texlink_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

texlink_frame_t *texlink_frame_create(const texlink_frame_desc_t *desc) {
  (void)desc;
  return NULL;
}

texlink_frame_t *texlink_frame_create_from_native_handle(
    const texlink_frame_native_desc_t *desc) {
  (void)desc;
  return NULL;
}

void texlink_frame_destroy(texlink_frame_t *frame) {
  free(frame);
}

int texlink_frame_should_flip_y(texlink_backend_t producer,
                                texlink_backend_t consumer) {
  if (producer == TEXLINK_BACKEND_UNKNOWN ||
      consumer == TEXLINK_BACKEND_UNKNOWN)
    return 0;
  if (producer == consumer)
    return 0;
  if (producer == TEXLINK_BACKEND_EGL && consumer == TEXLINK_BACKEND_VULKAN)
    return 1;
  if (producer == TEXLINK_BACKEND_VULKAN && consumer == TEXLINK_BACKEND_EGL)
    return 1;
  return 0;
}

texlink_meta_t texlink_frame_meta(texlink_frame_t *frame) {
  if (!frame) {
    texlink_meta_t zero = {0};
    return zero;
  }
  return frame->meta;
}

int texlink_frame_index(texlink_frame_t *frame) {
  return frame ? frame->index : -1;
}

int texlink_frame_get_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  (void)frame;
  (void)type;
  (void)out_handle;
  return -ENOTSUP;
}

int texlink_frame_dup_native_handle(texlink_frame_t *frame,
                                    texlink_native_handle_type_t type,
                                    texlink_native_handle_t *out_handle) {
  (void)frame;
  (void)type;
  (void)out_handle;
  return -ENOTSUP;
}

int texlink_native_handle_close(texlink_native_handle_t *handle) {
  if (!handle)
    return -EINVAL;
  if (!(handle->flags & TEXLINK_NATIVE_HANDLE_FLAG_OWNED))
    return -EPERM;
  if (handle->value.fd >= 0)
    close(handle->value.fd);
  memset(handle, 0, sizeof(*handle));
  handle->value.fd = -1;
  return 0;
}
