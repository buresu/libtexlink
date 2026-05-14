#include <texlink_d3d11.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 3
#define WIDTH 256
#define HEIGHT 256

typedef struct {
  IDXGISwapChain *swapchain;
  ID3D11Texture2D *backbuffer;
} window_target_t;

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
  if (target->backbuffer)
    target->backbuffer->lpVtbl->Release(target->backbuffer);
  if (target->swapchain)
    target->swapchain->lpVtbl->Release(target->swapchain);
}

static int create_window_target(ID3D11Device *device, HWND hwnd,
                                window_target_t *out) {
  IDXGIDevice *dxgi_device = NULL;
  IDXGIAdapter *adapter = NULL;
  IDXGIFactory *factory = NULL;
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
                                         (void **)&out->backbuffer);

done:
  if (factory)
    factory->lpVtbl->Release(factory);
  if (adapter)
    adapter->lpVtbl->Release(adapter);
  if (dxgi_device)
    dxgi_device->lpVtbl->Release(dxgi_device);
  return SUCCEEDED(hr) && out->swapchain && out->backbuffer ? 0 : -1;
}

static void present_texture(texlink_d3d11_texture_frame_t *texture_frame,
                            window_target_t *target) {
  ID3D11Device *device = texlink_d3d11_texture_frame_device(texture_frame);
  ID3D11DeviceContext *context = NULL;
  device->lpVtbl->GetImmediateContext(device, &context);
  if (!context)
    return;
  context->lpVtbl->CopyResource(
      context, (ID3D11Resource *)target->backbuffer,
      (ID3D11Resource *)texlink_d3d11_texture_frame_texture(texture_frame));
  context->lpVtbl->Flush(context);
  context->lpVtbl->Release(context);
  target->swapchain->lpVtbl->Present(target->swapchain, 1, 0);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "d3d11_example";

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "texlink D3D11 consumer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_D3D11,
      .timeout_ms = 5000,
  });
  if (!client || texlink_client_connect(client) != 0) {
    fprintf(stderr, "texlink_client_connect failed\n");
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  uint32_t frame_count = texlink_client_frame_count(client);
  if (frame_count > MAX_FRAMES)
    frame_count = MAX_FRAMES;

  ID3D11Device *device = NULL;
  if (create_device(&device) != 0) {
    fprintf(stderr, "D3D11CreateDevice failed\n");
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  texlink_d3d11_texture_frame_t *texture_frames[MAX_FRAMES] = {0};
  for (uint32_t i = 0; i < frame_count; i++) {
    texture_frames[i] =
        texlink_d3d11_texture_frame_import(&(texlink_d3d11_import_desc_t){
            .version = 1,
            .device = device,
            .frame = texlink_client_frame(client, i),
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d11_texture_frame_import failed: %s\n",
              texlink_d3d11_last_error_string());
      device->lpVtbl->Release(device);
      texlink_client_destroy(client);
      glfwTerminate();
      return 1;
    }
  }

  window_target_t target;
  if (create_window_target(device, glfwGetWin32Window(window), &target) != 0) {
    fprintf(stderr, "D3D11 swapchain creation failed\n");
    device->lpVtbl->Release(device);
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  printf("Connected to \"%s\" with %u D3D11 textures\n", name, frame_count);
  while (!glfwWindowShouldClose(window)) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    int idx = texlink_frame_index(frame);
    if (idx < 0 || (uint32_t)idx >= frame_count)
      idx = 0;

    present_texture(texture_frames[idx], &target);
    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  release_window_target(&target);
  for (uint32_t i = 0; i < frame_count; i++)
    texlink_d3d11_texture_frame_destroy(texture_frames[i]);
  device->lpVtbl->Release(device);
  texlink_client_destroy(client);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
