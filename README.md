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

# Windows — D3D12 + Vulkan
cmake -B build -DTEXLINK_WITH_D3D12=ON -DTEXLINK_WITH_VULKAN=ON
```

### Install
```sh
cmake --install build --prefix /usr/local
```

## License
MIT License
