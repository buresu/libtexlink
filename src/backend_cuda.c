#include <texlink_cuda.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct texlink_cuda_mapping {
  CUdeviceptr ptr;
  uint64_t size;
  struct texlink_cuda_mapping *next;
};

struct texlink_cuda_memory {
  CUmemGenericAllocationHandle handle;
  texlink_frame_t *frame;
  uint64_t size;
  struct texlink_cuda_mapping *mappings;
};

static uint64_t import_size(const texlink_cuda_import_desc_t *desc,
                            const texlink_meta_t *meta) {
  if (desc->size)
    return desc->size;
  if (meta->size)
    return meta->size;
  if (meta->stride && meta->height)
    return (uint64_t)meta->stride * meta->height;
  return 0;
}

texlink_cuda_memory_t *
texlink_cuda_memory_import(const texlink_cuda_import_desc_t *desc) {
  if (!desc || !desc->frame)
    return NULL;

  texlink_meta_t meta = texlink_frame_meta(desc->frame);
  uint64_t size = import_size(desc, &meta);
  if (size == 0)
    return NULL;

  texlink_native_handle_t fd_handle;
  if (texlink_frame_dup_native_handle(desc->frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
                                      &fd_handle) != 0)
    return NULL;

  texlink_cuda_memory_t *memory = calloc(1, sizeof(*memory));
  if (!memory) {
    texlink_native_handle_close(&fd_handle);
    return NULL;
  }

  CUresult result = cuMemImportFromShareableHandle(
      &memory->handle, (void *)(uintptr_t)fd_handle.value.fd,
      CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR);
  texlink_native_handle_close(&fd_handle);
  if (result != CUDA_SUCCESS) {
    free(memory);
    return NULL;
  }

  (void)desc->flags;
  memory->frame = desc->frame;
  memory->size = size;
  return memory;
}

CUmemGenericAllocationHandle
texlink_cuda_memory_allocation_handle(texlink_cuda_memory_t *memory) {
  return memory ? memory->handle : 0;
}

texlink_frame_t *texlink_cuda_memory_frame(texlink_cuda_memory_t *memory) {
  return memory ? memory->frame : NULL;
}

CUresult
texlink_cuda_memory_map_buffer(texlink_cuda_memory_t *memory,
                               const texlink_cuda_buffer_map_desc_t *desc,
                               void **out_ptr) {
  if (!memory || !out_ptr)
    return CUDA_ERROR_INVALID_VALUE;

  uint64_t offset = desc ? desc->offset : 0;
  uint64_t size = desc && desc->size ? desc->size : memory->size - offset;
  if (offset > memory->size || size == 0 || size > memory->size - offset)
    return CUDA_ERROR_INVALID_VALUE;

  CUdeviceptr ptr = 0;
  CUresult result = cuMemAddressReserve(&ptr, size, 0, 0, 0);
  if (result != CUDA_SUCCESS)
    return result;

  result = cuMemMap(ptr, size, offset, memory->handle, desc ? desc->flags : 0);
  if (result != CUDA_SUCCESS) {
    cuMemAddressFree(ptr, size);
    return result;
  }

  int device = 0;
  result = cuCtxGetDevice(&device);
  if (result != CUDA_SUCCESS) {
    cuMemUnmap(ptr, size);
    cuMemAddressFree(ptr, size);
    return result;
  }

  CUmemAccessDesc access = {
      .location = {.type = CU_MEM_LOCATION_TYPE_DEVICE, .id = device},
      .flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE,
  };
  result = cuMemSetAccess(ptr, size, &access, 1);
  if (result != CUDA_SUCCESS) {
    cuMemUnmap(ptr, size);
    cuMemAddressFree(ptr, size);
    return result;
  }

  struct texlink_cuda_mapping *mapping = calloc(1, sizeof(*mapping));
  if (!mapping) {
    cuMemUnmap(ptr, size);
    cuMemAddressFree(ptr, size);
    return CUDA_ERROR_OUT_OF_MEMORY;
  }
  mapping->ptr = ptr;
  mapping->size = size;
  mapping->next = memory->mappings;
  memory->mappings = mapping;

  *out_ptr = (void *)(uintptr_t)ptr;
  return CUDA_SUCCESS;
}

CUresult texlink_cuda_memory_unmap_buffer(texlink_cuda_memory_t *memory,
                                          void *ptr) {
  if (!memory || !ptr)
    return CUDA_ERROR_INVALID_VALUE;

  CUdeviceptr devptr = (CUdeviceptr)(uintptr_t)ptr;
  struct texlink_cuda_mapping **link = &memory->mappings;
  while (*link && (*link)->ptr != devptr)
    link = &(*link)->next;
  if (!*link)
    return CUDA_ERROR_INVALID_VALUE;

  struct texlink_cuda_mapping *mapping = *link;
  *link = mapping->next;
  CUresult result = cuMemUnmap(mapping->ptr, mapping->size);
  CUresult free_result = cuMemAddressFree(mapping->ptr, mapping->size);
  free(mapping);
  return result == CUDA_SUCCESS ? free_result : result;
}

void texlink_cuda_memory_destroy(texlink_cuda_memory_t *memory) {
  if (!memory)
    return;
  while (memory->mappings)
    texlink_cuda_memory_unmap_buffer(memory,
                                     (void *)(uintptr_t)memory->mappings->ptr);
  if (memory->handle)
    cuMemRelease(memory->handle);
  free(memory);
}
