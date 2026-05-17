#include <texlink_d3d12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WIDTH 256
#define HEIGHT 256
#define FRAME_COUNT 2

typedef struct {
  ID3D11Device *device;
  ID3D11DeviceContext *context;
  ID3D11Texture2D *texture;
  HANDLE shared_handle;
} d3d11_shared_texture_t;

typedef struct {
  ID3D12Device *device;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12DescriptorHeap *rtv_heap;
  IDXGISwapChain3 *swapchain;
  ID3D12Resource *backbuffers[2];
  ID3D12Fence *fence;
  HANDLE shared_fence;
  HANDLE fence_event;
  UINT rtv_size;
  uint64_t fence_value;
} d3d12_context_t;

static void release_d3d11_shared_texture(d3d11_shared_texture_t *shared) {
  if (shared->shared_handle)
    CloseHandle(shared->shared_handle);
  if (shared->texture)
    shared->texture->lpVtbl->Release(shared->texture);
  if (shared->context)
    shared->context->lpVtbl->Release(shared->context);
  if (shared->device)
    shared->device->lpVtbl->Release(shared->device);
}

static int create_d3d11_shared_texture(d3d11_shared_texture_t *shared) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  D3D_FEATURE_LEVEL selected = 0;
  D3D11_TEXTURE2D_DESC td;
  IDXGIResource1 *resource = NULL;
  HRESULT hr;

  memset(shared, 0, sizeof(*shared));
  hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels,
                         (UINT)(sizeof(levels) / sizeof(levels[0])),
                         D3D11_SDK_VERSION, &shared->device, &selected,
                         &shared->context);
  if (FAILED(hr))
    return -1;

  memset(&td, 0, sizeof(td));
  td.Width = WIDTH;
  td.Height = HEIGHT;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
  hr = shared->device->lpVtbl->CreateTexture2D(shared->device, &td, NULL,
                                               &shared->texture);
  if (FAILED(hr)) {
    td.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
    hr = shared->device->lpVtbl->CreateTexture2D(shared->device, &td, NULL,
                                                 &shared->texture);
    if (FAILED(hr))
      return -1;
  }

  hr = shared->texture->lpVtbl->QueryInterface(
      shared->texture, &IID_IDXGIResource1, (void **)&resource);
  if (FAILED(hr) || !resource)
    return -1;
  hr = resource->lpVtbl->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL,
                                            &shared->shared_handle);
  resource->lpVtbl->Release(resource);
  return SUCCEEDED(hr) && shared->shared_handle ? 0 : -1;
}

static void release_context(d3d12_context_t *ctx) {
  if (ctx->fence_event)
    CloseHandle(ctx->fence_event);
  if (ctx->shared_fence)
    CloseHandle(ctx->shared_fence);
  if (ctx->fence)
    ctx->fence->lpVtbl->Release(ctx->fence);
  if (ctx->rtv_heap)
    ctx->rtv_heap->lpVtbl->Release(ctx->rtv_heap);
  for (int i = 0; i < 2; i++) {
    if (ctx->backbuffers[i])
      ctx->backbuffers[i]->lpVtbl->Release(ctx->backbuffers[i]);
  }
  if (ctx->swapchain)
    ctx->swapchain->lpVtbl->Release(ctx->swapchain);
  if (ctx->list)
    ctx->list->lpVtbl->Release(ctx->list);
  if (ctx->allocator)
    ctx->allocator->lpVtbl->Release(ctx->allocator);
  if (ctx->queue)
    ctx->queue->lpVtbl->Release(ctx->queue);
  if (ctx->device)
    ctx->device->lpVtbl->Release(ctx->device);
}

static int signal_and_wait_gpu(d3d12_context_t *ctx, uint64_t *out_value) {
  uint64_t value = ++ctx->fence_value;
  if (FAILED(ctx->queue->lpVtbl->Signal(ctx->queue, ctx->fence, value)))
    return -1;
  if (ctx->fence->lpVtbl->GetCompletedValue(ctx->fence) < value) {
    if (FAILED(ctx->fence->lpVtbl->SetEventOnCompletion(ctx->fence, value,
                                                        ctx->fence_event)))
      return -1;
    WaitForSingleObject(ctx->fence_event, INFINITE);
  }
  if (out_value)
    *out_value = value;
  return 0;
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(d3d12_context_t *ctx, int idx);

static int init_context(d3d12_context_t *ctx, HWND hwnd) {
  D3D12_COMMAND_QUEUE_DESC qd;
  D3D12_DESCRIPTOR_HEAP_DESC hd;
  DXGI_SWAP_CHAIN_DESC1 sd;
  IDXGIFactory4 *factory = NULL;
  IDXGISwapChain1 *swapchain1 = NULL;
  HRESULT hr;
  memset(ctx, 0, sizeof(*ctx));
  memset(&qd, 0, sizeof(qd));
  memset(&hd, 0, sizeof(hd));
  memset(&sd, 0, sizeof(sd));

  hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                         (void **)&ctx->device);
  if (FAILED(hr))
    return -1;

  qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  hr = ctx->device->lpVtbl->CreateCommandQueue(ctx->device, &qd,
                                               &IID_ID3D12CommandQueue,
                                               (void **)&ctx->queue);
  if (FAILED(hr))
    return -1;

  hr = ctx->device->lpVtbl->CreateCommandAllocator(
      ctx->device, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,
      (void **)&ctx->allocator);
  if (FAILED(hr))
    return -1;

  hr = ctx->device->lpVtbl->CreateCommandList(
      ctx->device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ctx->allocator, NULL,
      &IID_ID3D12GraphicsCommandList, (void **)&ctx->list);
  if (FAILED(hr))
    return -1;
  ctx->list->lpVtbl->Close(ctx->list);

  hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  hd.NumDescriptors = FRAME_COUNT + 2;
  hr = ctx->device->lpVtbl->CreateDescriptorHeap(
      ctx->device, &hd, &IID_ID3D12DescriptorHeap, (void **)&ctx->rtv_heap);
  if (FAILED(hr))
    return -1;
  ctx->rtv_size = ctx->device->lpVtbl->GetDescriptorHandleIncrementSize(
      ctx->device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

  sd.Width = WIDTH;
  sd.Height = HEIGHT;
  sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  hr = CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory);
  if (FAILED(hr))
    return -1;
  hr = factory->lpVtbl->CreateSwapChainForHwnd(
      factory, (IUnknown *)ctx->queue, hwnd, &sd, NULL, NULL, &swapchain1);
  if (SUCCEEDED(hr)) {
    hr = swapchain1->lpVtbl->QueryInterface(
        swapchain1, &IID_IDXGISwapChain3, (void **)&ctx->swapchain);
  }
  if (swapchain1)
    swapchain1->lpVtbl->Release(swapchain1);
  if (factory)
    factory->lpVtbl->Release(factory);
  if (FAILED(hr))
    return -1;

  for (UINT i = 0; i < 2; i++) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv;
    hr = ctx->swapchain->lpVtbl->GetBuffer(
        ctx->swapchain, i, &IID_ID3D12Resource, (void **)&ctx->backbuffers[i]);
    if (FAILED(hr))
      return -1;
    rtv = rtv_handle(ctx, FRAME_COUNT + (int)i);
    ctx->device->lpVtbl->CreateRenderTargetView(ctx->device,
                                                ctx->backbuffers[i], NULL, rtv);
  }

  hr = ctx->device->lpVtbl->CreateFence(ctx->device, 0, D3D12_FENCE_FLAG_SHARED,
                                        &IID_ID3D12Fence, (void **)&ctx->fence);
  if (FAILED(hr))
    return -1;
  hr = ctx->device->lpVtbl->CreateSharedHandle(
      ctx->device, (ID3D12DeviceChild *)ctx->fence, NULL, GENERIC_ALL, NULL,
      &ctx->shared_fence);
  if (FAILED(hr) || !ctx->shared_fence)
    return -1;
  ctx->fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  return ctx->fence_event ? 0 : -1;
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(d3d12_context_t *ctx, int idx) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  ctx->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(ctx->rtv_heap,
                                                            &handle);
  handle.ptr += (SIZE_T)idx * ctx->rtv_size;
  return handle;
}

static void fill_bgra_pixels(unsigned char *dst, UINT row_pitch, float t) {
  float cx = 0.5f + 0.25f * cosf(t * 1.7f);
  float cy = 0.5f + 0.25f * sinf(t * 1.3f);
  for (UINT y = 0; y < HEIGHT; y++) {
    unsigned char *row = dst + (size_t)y * row_pitch;
    for (UINT x = 0; x < WIDTH; x++) {
      float u = (float)x / (float)(WIDTH - 1);
      float v = (float)y / (float)(HEIGHT - 1);
      float dx = u - cx;
      float dy = v - cy;
      float spot = 1.0f - fminf(1.0f, sqrtf(dx * dx + dy * dy) * 4.0f);
      row[x * 4 + 0] = (unsigned char)(255.0f * (0.20f + 0.55f * v));
      row[x * 4 + 1] = (unsigned char)(255.0f * (0.15f + 0.65f * spot));
      row[x * 4 + 2] = (unsigned char)(255.0f * (0.25f + 0.55f * u));
      row[x * 4 + 3] = 255;
    }
  }
}

static int upload_resource(d3d12_context_t *ctx, ID3D12Resource *resource,
                           float t, uint64_t *out_fence_value) {
  D3D12_RESOURCE_BARRIER barriers[4];
  UINT bb_idx = ctx->swapchain->lpVtbl->GetCurrentBackBufferIndex(ctx->swapchain);
  D3D12_RESOURCE_DESC texture_desc;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT rows = 0;
  UINT64 row_size = 0;
  UINT64 upload_size = 0;
  D3D12_HEAP_PROPERTIES heap_props;
  D3D12_RESOURCE_DESC upload_desc;
  D3D12_TEXTURE_COPY_LOCATION src;
  D3D12_TEXTURE_COPY_LOCATION dst;
  D3D12_TEXTURE_COPY_LOCATION backbuffer_dst;
  D3D12_RANGE range;
  ID3D12Resource *upload = NULL;
  ID3D12CommandList *lists[1];
  void *mapped = NULL;
  int ret = -1;

  resource->lpVtbl->GetDesc(resource, &texture_desc);
  ctx->device->lpVtbl->GetCopyableFootprints(ctx->device, &texture_desc, 0, 1,
                                             0, &footprint, &rows, &row_size,
                                             &upload_size);

  memset(&heap_props, 0, sizeof(heap_props));
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  heap_props.CreationNodeMask = 1;
  heap_props.VisibleNodeMask = 1;

  memset(&upload_desc, 0, sizeof(upload_desc));
  upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  upload_desc.Width = upload_size;
  upload_desc.Height = 1;
  upload_desc.DepthOrArraySize = 1;
  upload_desc.MipLevels = 1;
  upload_desc.SampleDesc.Count = 1;
  upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  if (FAILED(ctx->device->lpVtbl->CreateCommittedResource(
          ctx->device, &heap_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,
          (void **)&upload)))
    return -1;

  range.Begin = 0;
  range.End = 0;
  if (FAILED(upload->lpVtbl->Map(upload, 0, &range, &mapped)))
    goto done;
  fill_bgra_pixels((unsigned char *)mapped + footprint.Offset,
                   footprint.Footprint.RowPitch, t);
  upload->lpVtbl->Unmap(upload, 0, NULL);
  mapped = NULL;

  if (FAILED(ctx->allocator->lpVtbl->Reset(ctx->allocator)))
    goto done;
  if (FAILED(ctx->list->lpVtbl->Reset(ctx->list, ctx->allocator, NULL)))
    goto done;

  memset(barriers, 0, sizeof(barriers));
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = resource;
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1] = barriers[0];
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[2].Transition.pResource = ctx->backbuffers[bb_idx];
  barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[3] = barriers[2];
  barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&backbuffer_dst, 0, sizeof(backbuffer_dst));
  src.pResource = upload;
  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  src.PlacedFootprint = footprint;
  dst.pResource = resource;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  backbuffer_dst.pResource = ctx->backbuffers[bb_idx];
  backbuffer_dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  backbuffer_dst.SubresourceIndex = 0;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[2]);
  ctx->list->lpVtbl->CopyTextureRegion(ctx->list, &dst, 0, 0, 0, &src, NULL);
  ctx->list->lpVtbl->CopyTextureRegion(ctx->list, &backbuffer_dst, 0, 0, 0,
                                       &src, NULL);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[3]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    goto done;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  ctx->swapchain->lpVtbl->Present(ctx->swapchain, 1, 0);
  ret = signal_and_wait_gpu(ctx, out_fence_value);

done:
  if (mapped)
    upload->lpVtbl->Unmap(upload, 0, NULL);
  if (upload)
    upload->lpVtbl->Release(upload);
  return ret;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "d3d_interop";

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "texlink D3D12 producer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  d3d12_context_t ctx;
  if (init_context(&ctx, glfwGetWin32Window(window)) != 0) {
    fprintf(stderr, "D3D12 initialization failed\n");
    release_context(&ctx);
    glfwTerminate();
    return 1;
  }

  d3d11_shared_texture_t shared_textures[FRAME_COUNT];
  ID3D12Resource *resources[FRAME_COUNT] = {0};
  texlink_frame_t *frames[FRAME_COUNT] = {0};
  memset(shared_textures, 0, sizeof(shared_textures));
  for (int i = 0; i < FRAME_COUNT; i++) {
    if (create_d3d11_shared_texture(&shared_textures[i]) != 0) {
      fprintf(stderr, "D3D11 shared texture creation failed\n");
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }

    HRESULT hr = ctx.device->lpVtbl->OpenSharedHandle(
        ctx.device, shared_textures[i].shared_handle, &IID_ID3D12Resource,
        (void **)&resources[i]);
    if (FAILED(hr) || !resources[i]) {
      fprintf(stderr, "D3D12 OpenSharedHandle failed\n");
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }

    frames[i] = texlink_d3d12_frame_wrap_resource(
        &(texlink_d3d12_wrap_resource_desc_t){
            .device = ctx.device,
            .resource = resources[i],
            .shared_handle = shared_textures[i].shared_handle,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
        });
    if (!frames[i]) {
      fprintf(stderr, "texlink_d3d12_texture_frame_wrap_resource failed: %s\n",
              texlink_d3d12_last_error_string());
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }
    texlink_native_handle_t sync_handle = {
        .handle_type = TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
        .flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED,
        .value.ptr = ctx.shared_fence,
    };
    if (texlink_frame_set_sync_native_handle(frames[i], &sync_handle, 0) != 0) {
      fprintf(stderr, "texlink_frame_set_sync_native_handle failed\n");
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }
  }

  texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
      .name = name,
      .backend_type = TEXLINK_BACKEND_D3D12,
      .frames = frames,
      .frame_count = FRAME_COUNT,
  });
  if (!server || texlink_server_start(server) != 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    texlink_server_destroy(server);
    release_context(&ctx);
    glfwTerminate();
    return 1;
  }

  printf("Serving \"%s\" with %d D3D12 textures\n", name, FRAME_COUNT);
  while (!glfwWindowShouldClose(window)) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;

    int idx = texlink_frame_index(frame);
    if (idx < 0 || idx >= FRAME_COUNT)
      idx = 0;
    uint64_t fence_value = 0;
    if (upload_resource(&ctx, resources[idx], (float)glfwGetTime(),
                        &fence_value) != 0)
      break;
    texlink_frame_set_sync_value(frame, fence_value);
    texlink_server_end_frame(server, frame);

    glfwPollEvents();
    Sleep(16);
  }

  texlink_server_destroy(server);
  for (int i = 0; i < FRAME_COUNT; i++) {
    if (frames[i])
      texlink_frame_destroy(frames[i]);
    if (resources[i])
      resources[i]->lpVtbl->Release(resources[i]);
  }
  for (int i = 0; i < FRAME_COUNT; i++)
    release_d3d11_shared_texture(&shared_textures[i]);
  release_context(&ctx);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
