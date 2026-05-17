#include <texlink_d3d11.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAX_FRAMES 3
#define WIDTH 512
#define HEIGHT 512

typedef struct {
  IDXGISwapChain *swapchain;
  ID3D11Texture2D *backbuffer;
  ID3D11RenderTargetView *rtv;
} window_target_t;

typedef struct {
  ID3D11VertexShader *vs;
  ID3D11PixelShader *ps;
  ID3D11SamplerState *sampler;
  ID3D11Buffer *constants;
  ID3D11ShaderResourceView *srvs[MAX_FRAMES];
} render_resources_t;

static int wait_shared_fence(ID3D12Fence *fence, uint64_t value) {
  if (!fence || value == 0)
    return 0;
  if (fence->lpVtbl->GetCompletedValue(fence) >= value)
    return 0;
  HANDLE event_handle = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!event_handle)
    return -1;
  HRESULT hr =
      fence->lpVtbl->SetEventOnCompletion(fence, value, event_handle);
  if (FAILED(hr)) {
    CloseHandle(event_handle);
    return -1;
  }
  WaitForSingleObject(event_handle, INFINITE);
  CloseHandle(event_handle);
  return 0;
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
  if (FAILED(hr))
    goto done;
  hr = device->lpVtbl->CreateRenderTargetView(
      device, (ID3D11Resource *)out->backbuffer, NULL, &out->rtv);

done:
  if (factory)
    factory->lpVtbl->Release(factory);
  if (adapter)
    adapter->lpVtbl->Release(adapter);
  if (dxgi_device)
    dxgi_device->lpVtbl->Release(dxgi_device);
  return SUCCEEDED(hr) && out->swapchain && out->backbuffer && out->rtv ? 0
                                                                        : -1;
}

static void release_render_resources(render_resources_t *rr) {
  for (int i = 0; i < MAX_FRAMES; i++) {
    if (rr->srvs[i])
      rr->srvs[i]->lpVtbl->Release(rr->srvs[i]);
  }
  if (rr->constants)
    rr->constants->lpVtbl->Release(rr->constants);
  if (rr->sampler)
    rr->sampler->lpVtbl->Release(rr->sampler);
  if (rr->ps)
    rr->ps->lpVtbl->Release(rr->ps);
  if (rr->vs)
    rr->vs->lpVtbl->Release(rr->vs);
}

static int create_render_resources(ID3D11Device *device,
                                   render_resources_t *rr) {
  static const char *shader_src =
      "Texture2D tex0 : register(t0);\n"
      "SamplerState samp0 : register(s0);\n"
      "cbuffer Params : register(b0) { float flip_y; float3 pad; }\n"
      "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
      "VSOut vs_main(uint vid : SV_VertexID) {\n"
      "  float2 pos[3] = { float2(-1.0, -1.0), float2(-1.0, 3.0), float2(3.0, -1.0) };\n"
      "  VSOut o; o.pos = float4(pos[vid], 0.0, 1.0);\n"
      "  o.uv = float2((pos[vid].x + 1.0) * 0.5, 1.0 - (pos[vid].y + 1.0) * 0.5);\n"
      "  if (flip_y > 0.5) o.uv.y = 1.0 - o.uv.y;\n"
      "  return o;\n"
      "}\n"
      "float4 ps_main(VSOut i) : SV_Target { return tex0.Sample(samp0, i.uv); }\n";
  ID3DBlob *vs = NULL;
  ID3DBlob *ps = NULL;
  ID3DBlob *err = NULL;
  D3D11_SAMPLER_DESC sd;
  D3D11_BUFFER_DESC bd;
  HRESULT hr;

  memset(rr, 0, sizeof(*rr));
  hr = D3DCompile(shader_src, strlen(shader_src), NULL, NULL, NULL, "vs_main",
                  "vs_5_0", 0, 0, &vs, &err);
  if (FAILED(hr))
    goto done;
  hr = D3DCompile(shader_src, strlen(shader_src), NULL, NULL, NULL, "ps_main",
                  "ps_5_0", 0, 0, &ps, &err);
  if (FAILED(hr))
    goto done;
  hr = device->lpVtbl->CreateVertexShader(device, vs->lpVtbl->GetBufferPointer(vs),
                                          vs->lpVtbl->GetBufferSize(vs), NULL,
                                          &rr->vs);
  if (FAILED(hr))
    goto done;
  hr = device->lpVtbl->CreatePixelShader(device, ps->lpVtbl->GetBufferPointer(ps),
                                         ps->lpVtbl->GetBufferSize(ps), NULL,
                                         &rr->ps);
  if (FAILED(hr))
    goto done;

  memset(&sd, 0, sizeof(sd));
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  hr = device->lpVtbl->CreateSamplerState(device, &sd, &rr->sampler);
  if (FAILED(hr))
    goto done;

  memset(&bd, 0, sizeof(bd));
  bd.ByteWidth = 16;
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  hr = device->lpVtbl->CreateBuffer(device, &bd, NULL, &rr->constants);

done:
  if (err)
    err->lpVtbl->Release(err);
  if (vs)
    vs->lpVtbl->Release(vs);
  if (ps)
    ps->lpVtbl->Release(ps);
  return SUCCEEDED(hr) ? 0 : -1;
}

static int create_texture_srv(ID3D11Device *device, ID3D11Texture2D *texture,
                              ID3D11ShaderResourceView **out_srv) {
  D3D11_TEXTURE2D_DESC td;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd;
  texture->lpVtbl->GetDesc(texture, &td);
  memset(&sd, 0, sizeof(sd));
  sd.Format = td.Format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  return SUCCEEDED(device->lpVtbl->CreateShaderResourceView(
             device, (ID3D11Resource *)texture, &sd, out_srv))
             ? 0
             : -1;
}

static void present_texture(ID3D11Device *device, window_target_t *target,
                            render_resources_t *rr, int srv_idx, int flip_y) {
  ID3D11DeviceContext *context = NULL;
  D3D11_VIEWPORT viewport;
  float clear[4] = {0.02f, 0.02f, 0.025f, 1.0f};
  float constants[4] = {(float)flip_y, 0.0f, 0.0f, 0.0f};

  device->lpVtbl->GetImmediateContext(device, &context);
  if (!context)
    return;

  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = (float)WIDTH;
  viewport.Height = (float)HEIGHT;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;

  context->lpVtbl->ClearRenderTargetView(context, target->rtv, clear);
  context->lpVtbl->UpdateSubresource(context, (ID3D11Resource *)rr->constants,
                                     0, NULL, constants, 0, 0);
  context->lpVtbl->IASetInputLayout(context, NULL);
  context->lpVtbl->IASetPrimitiveTopology(
      context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->lpVtbl->RSSetViewports(context, 1, &viewport);
  context->lpVtbl->OMSetRenderTargets(context, 1, &target->rtv, NULL);
  context->lpVtbl->VSSetShader(context, rr->vs, NULL, 0);
  context->lpVtbl->VSSetConstantBuffers(context, 0, 1, &rr->constants);
  context->lpVtbl->PSSetShader(context, rr->ps, NULL, 0);
  context->lpVtbl->PSSetShaderResources(context, 0, 1, &rr->srvs[srv_idx]);
  context->lpVtbl->PSSetSamplers(context, 0, 1, &rr->sampler);
  context->lpVtbl->Draw(context, 3, 0);
  ID3D11ShaderResourceView *null_srv = NULL;
  context->lpVtbl->PSSetShaderResources(context, 0, 1, &null_srv);
  context->lpVtbl->Flush(context);
  context->lpVtbl->Release(context);
  target->swapchain->lpVtbl->Present(target->swapchain, 1, 0);
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
      glfwCreateWindow(WIDTH, HEIGHT, "texlink D3D11 consumer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
      .name = name,
      .backend_type = TEXLINK_BACKEND_D3D11,
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
  texlink_meta_t meta = texlink_client_meta(client);

  ID3D11Device *device = NULL;
  if (create_device(&device) != 0) {
    fprintf(stderr, "D3D11CreateDevice failed\n");
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  ID3D12Device *sync_device = NULL;
  D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                    (void **)&sync_device);

  render_resources_t render;
  if (create_render_resources(device, &render) != 0) {
    fprintf(stderr, "D3D11 render resource creation failed\n");
    if (sync_device)
      sync_device->lpVtbl->Release(sync_device);
    device->lpVtbl->Release(device);
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  texlink_d3d11_texture_frame_t *texture_frames[MAX_FRAMES] = {0};
  ID3D12Fence *sync_fences[MAX_FRAMES] = {0};
  for (uint32_t i = 0; i < frame_count; i++) {
    texlink_frame_t *source_frame = texlink_client_frame(client, i);
    texture_frames[i] =
        texlink_d3d11_texture_frame_import(&(texlink_d3d11_import_desc_t){
            .device = device,
            .frame = source_frame,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d11_texture_frame_import failed: %s\n",
              texlink_d3d11_last_error_string());
      release_render_resources(&render);
      if (sync_device)
        sync_device->lpVtbl->Release(sync_device);
      device->lpVtbl->Release(device);
      texlink_client_destroy(client);
      glfwTerminate();
      return 1;
    }
    if (create_texture_srv(device,
                           texlink_d3d11_texture_frame_texture(texture_frames[i]),
                           &render.srvs[i]) != 0) {
      fprintf(stderr, "D3D11 SRV creation failed\n");
      release_render_resources(&render);
      if (sync_device)
        sync_device->lpVtbl->Release(sync_device);
      device->lpVtbl->Release(device);
      texlink_client_destroy(client);
      glfwTerminate();
      return 1;
    }
    if (sync_device) {
      texlink_native_handle_t sync_handle;
      uint64_t sync_value;
      if (texlink_frame_get_sync_native_handle(
              source_frame, TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
              &sync_handle, &sync_value) == 0) {
        sync_device->lpVtbl->OpenSharedHandle(
            sync_device, (HANDLE)sync_handle.value.ptr, &IID_ID3D12Fence,
            (void **)&sync_fences[i]);
      }
    }
  }

  window_target_t target;
  if (create_window_target(device, glfwGetWin32Window(window), &target) != 0) {
    fprintf(stderr, "D3D11 swapchain creation failed\n");
    release_render_resources(&render);
    if (sync_device)
      sync_device->lpVtbl->Release(sync_device);
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

    if (wait_shared_fence(sync_fences[idx], texlink_frame_sync_value(frame)) !=
        0) {
      fprintf(stderr, "D3D12 fence wait failed\n");
      texlink_client_release_frame(client, frame);
      break;
    }
    int flip_y = texlink_frame_should_flip_y((texlink_backend_t)meta.backend_type,
                                             TEXLINK_BACKEND_D3D11);
    present_texture(device, &target, &render, idx, flip_y);
    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  release_window_target(&target);
  for (uint32_t i = 0; i < frame_count; i++)
    texlink_d3d11_texture_frame_destroy(texture_frames[i]);
  for (uint32_t i = 0; i < frame_count; i++) {
    if (sync_fences[i])
      sync_fences[i]->lpVtbl->Release(sync_fences[i]);
  }
  release_render_resources(&render);
  if (sync_device)
    sync_device->lpVtbl->Release(sync_device);
  device->lpVtbl->Release(device);
  texlink_client_destroy(client);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
