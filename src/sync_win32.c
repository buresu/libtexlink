#include "texlink_internal.h"

#include <errno.h>
#include <stdlib.h>

int texlink_export_sync_file(int dma_fd) {
  (void)dma_fd;
  return -1;
}

int texlink_wait_sync_file(int sync_fd, int timeout_ms) {
  (void)sync_fd;
  (void)timeout_ms;
  return 0;
}

static int normalize_range(uint64_t frame_size, uint64_t offset,
                           uint64_t requested_size, uint64_t *out_size) {
  if (offset > frame_size)
    return -EINVAL;

  uint64_t size = requested_size;
  if (size == 0)
    size = frame_size - offset;
  if (offset + size < offset || offset + size > frame_size)
    return -EINVAL;

  *out_size = size;
  return 0;
}

static int frame_is_mappable(texlink_frame_t *frame) {
  return frame->handle.handle_type == TEXLINK_NATIVE_HANDLE_OPAQUE_WIN32_HANDLE;
}

int texlink_frame_map(texlink_frame_t *frame, const texlink_map_desc_t *desc,
                      texlink_mapping_t *out_mapping) {
  if (!frame || !frame->win32_handle || !desc || !out_mapping)
    return -EINVAL;
  if (!frame_is_mappable(frame))
    return -ENOTSUP;
  if (!(desc->flags & (TEXLINK_MAP_READ | TEXLINK_MAP_WRITE)))
    return -EINVAL;

  if (frame->map_ptr) {
    out_mapping->data = frame->map_ptr;
    out_mapping->size = frame->map_length;
    out_mapping->stride = frame->meta.stride;
    return 0;
  }

  uint64_t map_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &map_size);
  if (ret != 0)
    return ret;
  if (map_size == 0)
    return -EINVAL;

  DWORD access = 0;
  if (desc->flags & TEXLINK_MAP_READ)
    access |= FILE_MAP_READ;
  if (desc->flags & TEXLINK_MAP_WRITE)
    access |= FILE_MAP_WRITE;

  SYSTEM_INFO info;
  GetSystemInfo(&info);
  uint64_t granularity = info.dwAllocationGranularity;
  uint64_t map_offset = desc->offset & ~(granularity - 1u);
  uint64_t delta = desc->offset - map_offset;
  uint64_t view_size = map_size + delta;

  void *base = MapViewOfFile(frame->win32_handle, access,
                             (DWORD)(map_offset >> 32), (DWORD)map_offset,
                             (SIZE_T)view_size);
  if (!base)
    return -EIO;

  frame->map_base = base;
  frame->map_ptr = (uint8_t *)base + delta;
  frame->map_size = (size_t)view_size;
  frame->map_length = map_size;
  frame->map_offset = desc->offset;
  frame->map_flags = desc->flags;

  out_mapping->data = frame->map_ptr;
  out_mapping->size = map_size;
  out_mapping->stride = frame->meta.stride;
  return 0;
}

int texlink_frame_unmap(texlink_frame_t *frame) {
  if (!frame)
    return -EINVAL;
  if (frame->active_access)
    return -EBUSY;
  if (!frame->map_base)
    return 0;

  BOOL ok = UnmapViewOfFile(frame->map_base);
  frame->map_base = NULL;
  frame->map_ptr = NULL;
  frame->map_size = 0;
  frame->map_length = 0;
  frame->map_offset = 0;
  frame->map_flags = 0;
  return ok ? 0 : -EIO;
}

int texlink_frame_is_mapped(texlink_frame_t *frame) {
  return frame && frame->map_ptr;
}

int texlink_frame_cpu_begin(texlink_frame_t *frame,
                            const texlink_map_desc_t *desc) {
  if (!frame || !desc)
    return -EINVAL;
  if (!frame_is_mappable(frame))
    return -ENOTSUP;
  if (!texlink_frame_is_mapped(frame))
    return -EINVAL;
  if (!(desc->flags & (TEXLINK_MAP_READ | TEXLINK_MAP_WRITE)))
    return -EINVAL;
  if (frame->active_access)
    return -EBUSY;

  uint64_t access_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &access_size);
  if (ret != 0)
    return ret;

  frame->active_access = desc->flags;
  frame->active_access_offset = desc->offset;
  frame->active_access_size = access_size;
  return 0;
}

int texlink_frame_cpu_end(texlink_frame_t *frame,
                          const texlink_map_desc_t *desc) {
  if (!frame || !desc)
    return -EINVAL;
  if (!frame_is_mappable(frame))
    return -ENOTSUP;
  if (!texlink_frame_is_mapped(frame))
    return -EINVAL;
  if (!frame->active_access || desc->flags != frame->active_access)
    return -EINVAL;

  uint64_t access_size = 0;
  int ret = normalize_range((uint64_t)frame->size, desc->offset, desc->size,
                            &access_size);
  if (ret != 0)
    return ret;
  if (desc->offset != frame->active_access_offset ||
      access_size != frame->active_access_size)
    return -EINVAL;

  frame->active_access = 0;
  frame->active_access_offset = 0;
  frame->active_access_size = 0;
  return 0;
}
