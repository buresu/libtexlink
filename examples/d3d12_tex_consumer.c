#include <texlink_d3d12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAX_FRAMES 3
#define WIDTH 256
#define HEIGHT 256

typedef struct {
  ID3D12Device *device;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12DescriptorHeap *rtv_heap;
  IDXGISwapChain3 *swapchain;
  ID3D12Resource *backbuffers[2];
  ID3D12Fence *fence;
  HANDLE fence_event;
  UINT rtv_size;
  uint64_t fence_value;
} d3d12_context_t;

static void release_context(d3d12_context_t *ctx) {
  if (ctx->fence_event)
    CloseHandle(ctx->fence_event);
  if (ctx->fence)
    ctx->fence->lpVtbl->Release(ctx->fence);
  for (int i = 0; i < 2; i++) {
    if (ctx->backbuffers[i])
      ctx->backbuffers[i]->lpVtbl->Release(ctx->backbuffers[i]);
  }
  if (ctx->swapchain)
    ctx->swapchain->lpVtbl->Release(ctx->swapchain);
  if (ctx->rtv_heap)
    ctx->rtv_heap->lpVtbl->Release(ctx->rtv_heap);
  if (ctx->list)
    ctx->list->lpVtbl->Release(ctx->list);
  if (ctx->allocator)
    ctx->allocator->lpVtbl->Release(ctx->allocator);
  if (ctx->queue)
    ctx->queue->lpVtbl->Release(ctx->queue);
  if (ctx->device)
    ctx->device->lpVtbl->Release(ctx->device);
}

static int wait_gpu(d3d12_context_t *ctx) {
  uint64_t value = ++ctx->fence_value;
  if (FAILED(ctx->queue->lpVtbl->Signal(ctx->queue, ctx->fence, value)))
    return -1;
  if (ctx->fence->lpVtbl->GetCompletedValue(ctx->fence) < value) {
    if (FAILED(ctx->fence->lpVtbl->SetEventOnCompletion(ctx->fence, value,
                                                        ctx->fence_event)))
      return -1;
    WaitForSingleObject(ctx->fence_event, INFINITE);
  }
  return 0;
}

static D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(d3d12_context_t *ctx, int idx) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  ctx->rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(ctx->rtv_heap,
                                                            &handle);
  handle.ptr += (SIZE_T)idx * ctx->rtv_size;
  return handle;
}

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
  hd.NumDescriptors = 2;
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
    rtv = rtv_handle(ctx, (int)i);
    ctx->device->lpVtbl->CreateRenderTargetView(ctx->device,
                                                ctx->backbuffers[i], NULL, rtv);
  }

  hr = ctx->device->lpVtbl->CreateFence(ctx->device, 0, D3D12_FENCE_FLAG_NONE,
                                        &IID_ID3D12Fence, (void **)&ctx->fence);
  if (FAILED(hr))
    return -1;
  ctx->fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);
  return ctx->fence_event ? 0 : -1;
}

static int present_color(d3d12_context_t *ctx, const unsigned char bgra[4]) {
  D3D12_RESOURCE_BARRIER barriers[2];
  UINT bb_idx = ctx->swapchain->lpVtbl->GetCurrentBackBufferIndex(ctx->swapchain);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(ctx, (int)bb_idx);
  ID3D12CommandList *lists[1];
  float color[4] = {
      bgra[2] / 255.0f,
      bgra[1] / 255.0f,
      bgra[0] / 255.0f,
      bgra[3] / 255.0f,
  };

  if (FAILED(ctx->allocator->lpVtbl->Reset(ctx->allocator)))
    return -1;
  if (FAILED(ctx->list->lpVtbl->Reset(ctx->list, ctx->allocator, NULL)))
    return -1;

  memset(barriers, 0, sizeof(barriers));
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = ctx->backbuffers[bb_idx];
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1] = barriers[0];
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->ClearRenderTargetView(ctx->list, rtv, color, 0, NULL);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    return -1;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  ctx->swapchain->lpVtbl->Present(ctx->swapchain, 1, 0);
  return wait_gpu(ctx);
}

static int read_first_pixel(d3d12_context_t *ctx, ID3D12Resource *resource,
                            unsigned char out_bgra[4]) {
  D3D12_RESOURCE_DESC rd;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT rows = 0;
  UINT64 row_size = 0;
  UINT64 total_size = 0;
  D3D12_HEAP_PROPERTIES heap_props;
  D3D12_RESOURCE_DESC buffer_desc;
  D3D12_TEXTURE_COPY_LOCATION src;
  D3D12_TEXTURE_COPY_LOCATION dst;
  D3D12_RESOURCE_BARRIER barriers[2];
  D3D12_RANGE read_range;
  ID3D12Resource *readback = NULL;
  ID3D12CommandList *lists[1];
  void *mapped = NULL;
  int ret = -1;

  resource->lpVtbl->GetDesc(resource, &rd);
  ctx->device->lpVtbl->GetCopyableFootprints(ctx->device, &rd, 0, 1, 0,
                                             &footprint, &rows, &row_size,
                                             &total_size);

  memset(&heap_props, 0, sizeof(heap_props));
  heap_props.Type = D3D12_HEAP_TYPE_READBACK;
  heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  heap_props.CreationNodeMask = 1;
  heap_props.VisibleNodeMask = 1;

  memset(&buffer_desc, 0, sizeof(buffer_desc));
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = total_size;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.SampleDesc.Count = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

  if (FAILED(ctx->device->lpVtbl->CreateCommittedResource(
          ctx->device, &heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,
          (void **)&readback)))
    return -1;

  if (FAILED(ctx->allocator->lpVtbl->Reset(ctx->allocator)))
    goto done;
  if (FAILED(ctx->list->lpVtbl->Reset(ctx->list, ctx->allocator, NULL)))
    goto done;

  memset(barriers, 0, sizeof(barriers));
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = resource;
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1] = barriers[0];
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  src.pResource = resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = readback;
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->CopyTextureRegion(ctx->list, &dst, 0, 0, 0, &src, NULL);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    goto done;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  if (wait_gpu(ctx) != 0)
    goto done;

  read_range.Begin = 0;
  read_range.End = 4;
  if (SUCCEEDED(readback->lpVtbl->Map(readback, 0, &read_range, &mapped))) {
    const unsigned char *p = mapped;
    out_bgra[0] = p[0];
    out_bgra[1] = p[1];
    out_bgra[2] = p[2];
    out_bgra[3] = p[3];
    read_range.Begin = 0;
    read_range.End = 0;
    readback->lpVtbl->Unmap(readback, 0, &read_range);
    ret = 0;
  }

done:
  if (readback)
    readback->lpVtbl->Release(readback);
  return ret;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "d3d12_example";

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "texlink D3D12 consumer", NULL, NULL);
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

  texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_D3D12,
      .timeout_ms = 5000,
  });
  if (!client || texlink_client_connect(client) != 0) {
    fprintf(stderr, "texlink_client_connect failed\n");
    texlink_client_destroy(client);
    release_context(&ctx);
    glfwTerminate();
    return 1;
  }

  uint32_t frame_count = texlink_client_frame_count(client);
  if (frame_count > MAX_FRAMES)
    frame_count = MAX_FRAMES;

  texlink_d3d12_texture_frame_t *texture_frames[MAX_FRAMES] = {0};
  for (uint32_t i = 0; i < frame_count; i++) {
    texture_frames[i] =
        texlink_d3d12_texture_frame_import(&(texlink_d3d12_import_desc_t){
            .version = 1,
            .device = ctx.device,
            .frame = texlink_client_frame(client, i),
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d12_texture_frame_import failed: %s\n",
              texlink_d3d12_last_error_string());
      texlink_client_destroy(client);
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }
  }

  printf("Connected to \"%s\" with %u D3D12 textures\n", name, frame_count);
  while (!glfwWindowShouldClose(window)) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    int idx = texlink_frame_index(frame);
    if (idx < 0 || (uint32_t)idx >= frame_count)
      idx = 0;

    unsigned char bgra[4] = {0};
    ID3D12Resource *resource =
        texlink_d3d12_texture_frame_resource(texture_frames[idx]);
    read_first_pixel(&ctx, resource, bgra);
    present_color(&ctx, bgra);
    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  for (uint32_t i = 0; i < frame_count; i++)
    texlink_d3d12_texture_frame_destroy(texture_frames[i]);
  texlink_client_destroy(client);
  release_context(&ctx);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
