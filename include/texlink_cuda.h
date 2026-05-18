#pragma once

#include <cuda.h>

#include <texlink.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct texlink_cuda_memory texlink_cuda_memory_t;

typedef struct {
  texlink_frame_t *frame;
  uint64_t size;
  unsigned int flags;
} texlink_cuda_import_desc_t;

typedef struct {
  uint64_t offset;
  uint64_t size;
  unsigned int flags;
} texlink_cuda_buffer_map_desc_t;

texlink_cuda_memory_t *
texlink_cuda_memory_import(const texlink_cuda_import_desc_t *desc);
CUmemGenericAllocationHandle
texlink_cuda_memory_allocation_handle(texlink_cuda_memory_t *memory);
texlink_frame_t *texlink_cuda_memory_frame(texlink_cuda_memory_t *memory);

CUresult
texlink_cuda_memory_map_buffer(texlink_cuda_memory_t *memory,
                               const texlink_cuda_buffer_map_desc_t *desc,
                               void **out_ptr);
CUresult texlink_cuda_memory_unmap_buffer(texlink_cuda_memory_t *memory,
                                          void *ptr);

void texlink_cuda_memory_destroy(texlink_cuda_memory_t *memory);

#ifdef __cplusplus
}
#endif
