#include <texlink_d3d12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <dxgi1_4.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WIDTH 256
#define HEIGHT 256
#define FRAME_COUNT 2

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

  hr = ctx->device->lpVtbl->CreateFence(ctx->device, 0, D3D12_FENCE_FLAG_NONE,
                                        &IID_ID3D12Fence, (void **)&ctx->fence);
  if (FAILED(hr))
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

static int clear_resource(d3d12_context_t *ctx, ID3D12Resource *resource,
                          int idx, float t) {
  D3D12_RESOURCE_BARRIER barriers[4];
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(ctx, idx);
  UINT bb_idx = ctx->swapchain->lpVtbl->GetCurrentBackBufferIndex(ctx->swapchain);
  D3D12_CPU_DESCRIPTOR_HANDLE window_rtv =
      rtv_handle(ctx, FRAME_COUNT + (int)bb_idx);
  ID3D12CommandList *lists[1];
  float color[4] = {
      0.5f + 0.5f * t,
      0.15f,
      1.0f - (0.5f + 0.5f * t),
      1.0f,
  };

  ctx->device->lpVtbl->CreateRenderTargetView(ctx->device, resource, NULL, rtv);
  if (FAILED(ctx->allocator->lpVtbl->Reset(ctx->allocator)))
    return -1;
  if (FAILED(ctx->list->lpVtbl->Reset(ctx->list, ctx->allocator, NULL)))
    return -1;

  memset(barriers, 0, sizeof(barriers));
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = resource;
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1] = barriers[0];
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[2].Transition.pResource = ctx->backbuffers[bb_idx];
  barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[3] = barriers[2];
  barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[2]);
  ctx->list->lpVtbl->ClearRenderTargetView(ctx->list, rtv, color, 0, NULL);
  ctx->list->lpVtbl->ClearRenderTargetView(ctx->list, window_rtv, color, 0,
                                           NULL);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[3]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    return -1;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  ctx->swapchain->lpVtbl->Present(ctx->swapchain, 1, 0);
  return wait_gpu(ctx);
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

  texlink_d3d12_texture_frame_t *texture_frames[FRAME_COUNT] = {0};
  texlink_frame_t *frames[FRAME_COUNT] = {0};
  for (int i = 0; i < FRAME_COUNT; i++) {
    texture_frames[i] = texlink_d3d12_texture_frame_create(
        &(texlink_d3d12_texture_frame_desc_t){
            .version = 1,
            .device = ctx.device,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
            .resource_flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
            .initial_state = D3D12_RESOURCE_STATE_COMMON,
            .heap_type = D3D12_HEAP_TYPE_DEFAULT,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d12_texture_frame_create failed: %s\n",
              texlink_d3d12_last_error_string());
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }
    frames[i] = texlink_d3d12_texture_frame_frame(texture_frames[i]);
  }

  texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_D3D12,
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
    float phase = 0.5f + 0.5f * sinf((float)glfwGetTime() * 1.6f);
    if (clear_resource(&ctx, texlink_d3d12_texture_frame_resource(
                                 texture_frames[idx]),
                       idx, phase) != 0)
      break;
    texlink_server_end_frame(server, frame);

    glfwPollEvents();
    Sleep(16);
  }

  texlink_server_destroy(server);
  for (int i = 0; i < FRAME_COUNT; i++)
    texlink_d3d12_texture_frame_destroy(texture_frames[i]);
  release_context(&ctx);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
