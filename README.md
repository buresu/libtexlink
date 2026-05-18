# libtexlink

> **[WIP]** Cross-platform GPU texture and buffer sharing library for inter-process communication.

libtexlink allows one process (producer) to share GPU textures or buffers with another process (consumer) with minimal copies.

The library abstracts platform-specific external memory handles behind a unified C API, enabling efficient GPU resource sharing between processes running on the same system and GPU.

**Supported backends**

| Backend | Linux | Windows |
|---------|:-----:|:-------:|
| CPU (shared memory) | ✓ | ✓ |
| EGL / OpenGL | ✓ | |
| WGL / OpenGL | | ✓ |
| Vulkan | ✓ | ✓ |
| CUDA import | ✓ | |
| Direct3D 11 | | ✓ |
| Direct3D 12 | | ✓ |

## How it works

libtexlink shares GPU textures between processes in three steps.

### 1. Native handle export

The producer exports each texture as a platform-native handle:

- **Linux** — DMA-BUF file descriptor (`/dev/dma_heap` or GBM)
- **Windows** — Win32 shared `HANDLE` (D3D11/D3D12 shared resource or opaque NT handle)

### 2. Handle transfer

At connect time the producer sends the native handles to the consumer over a Unix domain socket (Linux) or named pipe (Windows) using `SCM_RIGHTS` / `DuplicateHandle`. After this, both processes hold a reference to the same underlying GPU memory — no data copy occurs.

A small shared memory region (`texlink_shm_t`) carries frame metadata and the current buffer index. The producer writes a frame notification message to the socket each time a new frame is ready, which the consumer reads via `texlink_client_acquire_frame`.

### 3. GPU synchronization

To avoid reading a frame that the GPU is still writing:

- **Linux** — sync FD exported via `DMA_BUF_IOCTL_EXPORT_SYNC_FILE`, polled with `poll(2)`
- **Windows** — D3D12 fence handle shared between processes, waited with `ID3D12Fence::SetEventOnCompletion`

## Build

### Requirements

- CMake 3.21+
- C17 compiler
- **Linux**: GBM (`libgbm-dev`)
- **Windows**: MSVC or Clang-CL, Windows SDK

### Configure and build

```sh
cmake -B build
cmake --build build
```

Enable the backends you need:

```sh
# Linux — EGL + Vulkan
cmake -B build -DTEXLINK_WITH_EGL=ON -DTEXLINK_WITH_VULKAN=ON

# Linux — Vulkan dma-buf export + CUDA import
cmake -B build -DTEXLINK_WITH_VULKAN=ON -DTEXLINK_WITH_CUDA=ON

# Windows — D3D12 + Vulkan
cmake -B build -DTEXLINK_WITH_D3D12=ON -DTEXLINK_WITH_VULKAN=ON
```

## Linux Vulkan + CUDA dma-buf

On Linux, Vulkan is the central GPU sharing backend. `texlink_vulkan`
can create exportable `VkImage` and `VkBuffer` allocations, wrap existing
Vulkan allocations, and expose the shared memory through the common
`texlink_frame_t` metadata and DMA-BUF fd handle.

CUDA is an import backend. It does not export DMA-BUF memory from CUDA
allocations. `texlink_cuda` imports a `texlink_frame_t` carrying a
`TEXLINK_NATIVE_HANDLE_DMA_BUF_FD` and maps buffer resources to a CUDA
device pointer with the CUDA driver virtual memory APIs. Prefer
`texlink_vk_buffer_frame_create` for CUDA kernel read/write interop.
Vulkan images are exportable for Vulkan/EGL/DRM consumers; CUDA image
array mapping depends on CUDA driver support and is intentionally not
promised by the current C API.

For texture consumers, build both EGL and CUDA support and run the CUDA
texture sample against an existing texture producer:

```sh
cmake -B build -DTEXLINK_WITH_EGL=ON -DTEXLINK_WITH_VULKAN=ON -DTEXLINK_WITH_CUDA=ON
cmake --build build

# Terminal 1
./build/vulkan_tex_producer

# Terminal 2
./build/cuda_tex_consumer texlink
```

`cuda_tex_consumer` imports the producer's DMA-BUF texture as an EGLImage
and registers it with CUDA using CUDA EGL interop, so it can receive the
textures produced by the existing Vulkan/EGL texture examples.

For CUDA-written texture output, run the CUDA producer and any existing
texture consumer:

```sh
# Terminal 1
./build/cuda_tex_producer

# Terminal 2
./build/vulkan_tex_consumer
# or
./build/egl_tex_consumer
```

`cuda_tex_producer` still follows the Linux sharing model: EGL creates the
exportable DMA-BUF texture, CUDA imports that EGLImage and writes pixels,
then texlink publishes the frame for Vulkan/EGL consumers.

### Install
```sh
cmake --install build --prefix /usr/local
```

## License
MIT License
