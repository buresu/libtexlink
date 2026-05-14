#include <texlink_d3d11.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3d12.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define WIDTH 256
#define HEIGHT 256
#define FRAME_COUNT 2

typedef struct {
  IDXGISwapChain *swapchain;
  ID3D11RenderTargetView *rtv;
} window_target_t;

typedef struct {
  ID3D12Device *device;
  ID3D12Fence *fence;
  HANDLE shared_fence;
  uint64_t value;
} fence_bridge_t;

static void release_fence_bridge(fence_bridge_t *bridge) {
  if (bridge->shared_fence)
    CloseHandle(bridge->shared_fence);
  if (bridge->fence)
    bridge->fence->lpVtbl->Release(bridge->fence);
  if (bridge->device)
    bridge->device->lpVtbl->Release(bridge->device);
}

static int create_fence_bridge(fence_bridge_t *bridge) {
  HRESULT hr;
  memset(bridge, 0, sizeof(*bridge));

  hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                         (void **)&bridge->device);
  if (FAILED(hr))
    return -1;
  hr = bridge->device->lpVtbl->CreateFence(
      bridge->device, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence,
      (void **)&bridge->fence);
  if (FAILED(hr))
    return -1;
  hr = bridge->device->lpVtbl->CreateSharedHandle(
      bridge->device, (ID3D12DeviceChild *)bridge->fence, NULL, GENERIC_ALL,
      NULL, &bridge->shared_fence);
  return SUCCEEDED(hr) && bridge->shared_fence ? 0 : -1;
}

static uint64_t signal_fence_bridge(fence_bridge_t *bridge) {
  uint64_t value = ++bridge->value;
  if (FAILED(bridge->fence->lpVtbl->Signal(bridge->fence, value)))
    return 0;
  return value;
}

static int create_device(ID3D11Device **out_device) {
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_1,
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };
  ID3D11DeviceContext *context = NULL;
  D3D_FEATURE_LEVEL selected = 0;
  HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
                                 levels,
                                 (UINT)(sizeof(levels) / sizeof(levels[0])),
                                 D3D11_SDK_VERSION, out_device, &selected,
                                 &context);
  if (context)
    context->lpVtbl->Release(context);
  return SUCCEEDED(hr) ? 0 : -1;
}

static void release_window_target(window_target_t *target) {
  if (target->rtv)
    target->rtv->lpVtbl->Release(target->rtv);
  if (target->swapchain)
    target->swapchain->lpVtbl->Release(target->swapchain);
}

static int create_window_target(ID3D11Device *device, HWND hwnd,
                                window_target_t *out) {
  IDXGIDevice *dxgi_device = NULL;
  IDXGIAdapter *adapter = NULL;
  IDXGIFactory *factory = NULL;
  ID3D11Texture2D *backbuffer = NULL;
  DXGI_SWAP_CHAIN_DESC sd;
  HRESULT hr;

  memset(out, 0, sizeof(*out));
  memset(&sd, 0, sizeof(sd));
  sd.BufferDesc.Width = WIDTH;
  sd.BufferDesc.Height = HEIGHT;
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.SampleDesc.Count = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.BufferCount = 2;
  sd.OutputWindow = hwnd;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  hr = device->lpVtbl->QueryInterface(device, &IID_IDXGIDevice,
                                      (void **)&dxgi_device);
  if (FAILED(hr))
    goto done;
  hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, &adapter);
  if (FAILED(hr))
    goto done;
  hr = adapter->lpVtbl->GetParent(adapter, &IID_IDXGIFactory,
                                  (void **)&factory);
  if (FAILED(hr))
    goto done;
  hr = factory->lpVtbl->CreateSwapChain(factory, (IUnknown *)device, &sd,
                                        &out->swapchain);
  if (FAILED(hr))
    goto done;
  hr = out->swapchain->lpVtbl->GetBuffer(out->swapchain, 0,
                                         &IID_ID3D11Texture2D,
                                         (void **)&backbuffer);
  if (FAILED(hr))
    goto done;
  hr = device->lpVtbl->CreateRenderTargetView(
      device, (ID3D11Resource *)backbuffer, NULL, &out->rtv);

done:
  if (backbuffer)
    backbuffer->lpVtbl->Release(backbuffer);
  if (factory)
    factory->lpVtbl->Release(factory);
  if (adapter)
    adapter->lpVtbl->Release(adapter);
  if (dxgi_device)
    dxgi_device->lpVtbl->Release(dxgi_device);
  return SUCCEEDED(hr) && out->swapchain && out->rtv ? 0 : -1;
}

static void clear_texture(texlink_d3d11_texture_frame_t *texture_frame,
                          ID3D11RenderTargetView *window_rtv, float t) {
  ID3D11Device *device = texlink_d3d11_texture_frame_device(texture_frame);
  ID3D11Texture2D *texture =
      texlink_d3d11_texture_frame_texture(texture_frame);
  ID3D11DeviceContext *context = NULL;
  ID3D11RenderTargetView *rtv = NULL;

  device->lpVtbl->GetImmediateContext(device, &context);
  if (!context)
    return;

  if (SUCCEEDED(device->lpVtbl->CreateRenderTargetView(device,
                                                       (ID3D11Resource *)texture,
                                                       NULL, &rtv))) {
    float color[4] = {
        0.5f + 0.5f * t,
        0.2f,
        1.0f - (0.5f + 0.5f * t),
        1.0f,
    };
    context->lpVtbl->ClearRenderTargetView(context, rtv, color);
    if (window_rtv)
      context->lpVtbl->ClearRenderTargetView(context, window_rtv, color);
    context->lpVtbl->Flush(context);
    rtv->lpVtbl->Release(rtv);
  }
  context->lpVtbl->Release(context);
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
      glfwCreateWindow(WIDTH, HEIGHT, "texlink D3D11 producer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  ID3D11Device *device = NULL;
  if (create_device(&device) != 0) {
    fprintf(stderr, "D3D11CreateDevice failed\n");
    glfwTerminate();
    return 1;
  }

  fence_bridge_t bridge;
  if (create_fence_bridge(&bridge) != 0) {
    fprintf(stderr, "D3D12 fence bridge creation failed\n");
    device->lpVtbl->Release(device);
    glfwTerminate();
    return 1;
  }

  texlink_d3d11_texture_frame_t *texture_frames[FRAME_COUNT] = {0};
  texlink_frame_t *frames[FRAME_COUNT] = {0};
  for (int i = 0; i < FRAME_COUNT; i++) {
    texture_frames[i] = texlink_d3d11_texture_frame_create(
        &(texlink_d3d11_texture_frame_desc_t){
            .version = 1,
            .device = device,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
            .bind_flags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
            .misc_flags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d11_texture_frame_create failed: %s\n",
              texlink_d3d11_last_error_string());
      device->lpVtbl->Release(device);
      glfwTerminate();
      return 1;
    }
    frames[i] = texlink_d3d11_texture_frame_frame(texture_frames[i]);
    texlink_native_handle_t sync_handle = {
        .version = 1,
        .type = TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
        .flags = TEXLINK_NATIVE_HANDLE_FLAG_BORROWED,
        .value.ptr = bridge.shared_fence,
    };
    if (texlink_frame_set_sync_native_handle(frames[i], &sync_handle, 0) != 0) {
      fprintf(stderr, "texlink_frame_set_sync_native_handle failed\n");
      release_fence_bridge(&bridge);
      device->lpVtbl->Release(device);
      glfwTerminate();
      return 1;
    }
  }

  window_target_t target;
  if (create_window_target(device, glfwGetWin32Window(window), &target) != 0) {
    fprintf(stderr, "D3D11 swapchain creation failed\n");
    release_fence_bridge(&bridge);
    device->lpVtbl->Release(device);
    glfwTerminate();
    return 1;
  }

  texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_D3D11,
      .frames = frames,
      .frame_count = FRAME_COUNT,
  });
  if (!server || texlink_server_start(server) != 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    texlink_server_destroy(server);
    for (int i = 0; i < FRAME_COUNT; i++)
      texlink_d3d11_texture_frame_destroy(texture_frames[i]);
    release_window_target(&target);
    release_fence_bridge(&bridge);
    device->lpVtbl->Release(device);
    glfwTerminate();
    return 1;
  }

  printf("Serving \"%s\" with %d D3D11 textures\n", name, FRAME_COUNT);
  while (!glfwWindowShouldClose(window)) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;

    int idx = texlink_frame_index(frame);
    if (idx < 0 || idx >= FRAME_COUNT)
      idx = 0;
    float phase = 0.5f + 0.5f * sinf((float)glfwGetTime() * 1.6f);
    clear_texture(texture_frames[idx], target.rtv, phase);
    texlink_frame_set_sync_value(frame, signal_fence_bridge(&bridge));
    texlink_server_end_frame(server, frame);
    target.swapchain->lpVtbl->Present(target.swapchain, 1, 0);

    glfwPollEvents();
    Sleep(16);
  }

  texlink_server_destroy(server);
  release_window_target(&target);
  for (int i = 0; i < FRAME_COUNT; i++)
    texlink_d3d11_texture_frame_destroy(texture_frames[i]);
  release_fence_bridge(&bridge);
  device->lpVtbl->Release(device);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
