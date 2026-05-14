#include <texlink_d3d12.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define MAX_FRAMES 3
#define WIDTH 512
#define HEIGHT 512

static int wait_shared_fence(ID3D12Fence *fence, uint64_t value) {
  if (!fence || value == 0)
    return 0;
  if (fence->lpVtbl->GetCompletedValue(fence) >= value)
    return 0;
  HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
  if (!event)
    return -1;
  HRESULT hr = fence->lpVtbl->SetEventOnCompletion(fence, value, event);
  if (SUCCEEDED(hr))
    WaitForSingleObject(event, 5000);
  CloseHandle(event);
  return SUCCEEDED(hr) ? 0 : -1;
}

typedef struct {
  ID3D12Device *device;
  ID3D12CommandQueue *queue;
  ID3D12CommandAllocator *allocator;
  ID3D12GraphicsCommandList *list;
  ID3D12DescriptorHeap *rtv_heap;
  ID3D12DescriptorHeap *srv_heap;
  ID3D12RootSignature *root_signature;
  ID3D12PipelineState *pso;
  IDXGISwapChain3 *swapchain;
  ID3D12Resource *backbuffers[2];
  ID3D12Fence *fence;
  HANDLE fence_event;
  UINT rtv_size;
  UINT srv_size;
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
  if (ctx->srv_heap)
    ctx->srv_heap->lpVtbl->Release(ctx->srv_heap);
  if (ctx->pso)
    ctx->pso->lpVtbl->Release(ctx->pso);
  if (ctx->root_signature)
    ctx->root_signature->lpVtbl->Release(ctx->root_signature);
  if (ctx->list)
    ctx->list->lpVtbl->Release(ctx->list);
  if (ctx->allocator)
    ctx->allocator->lpVtbl->Release(ctx->allocator);
  if (ctx->queue)
    ctx->queue->lpVtbl->Release(ctx->queue);
  if (ctx->device)
    ctx->device->lpVtbl->Release(ctx->device);
}

static int create_texture_pipeline(d3d12_context_t *ctx) {
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
  D3D12_ROOT_PARAMETER params[2];
  D3D12_DESCRIPTOR_RANGE range;
  D3D12_STATIC_SAMPLER_DESC sampler;
  D3D12_ROOT_SIGNATURE_DESC rs_desc;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
  ID3DBlob *vs = NULL;
  ID3DBlob *ps = NULL;
  ID3DBlob *rs = NULL;
  ID3DBlob *err = NULL;
  HRESULT hr;

  memset(&range, 0, sizeof(range));
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart =
      D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

  memset(params, 0, sizeof(params));
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = 1;
  params[0].DescriptorTable.pDescriptorRanges = &range;
  params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 4;
  params[1].Constants.ShaderRegister = 0;
  params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

  memset(&sampler, 0, sizeof(sampler));
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

  memset(&rs_desc, 0, sizeof(rs_desc));
  rs_desc.NumParameters = 2;
  rs_desc.pParameters = params;
  rs_desc.NumStaticSamplers = 1;
  rs_desc.pStaticSamplers = &sampler;
  rs_desc.Flags =
      D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  hr = D3D12SerializeRootSignature(&rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs,
                                   &err);
  if (FAILED(hr))
    goto done;
  hr = ctx->device->lpVtbl->CreateRootSignature(
      ctx->device, 0, rs->lpVtbl->GetBufferPointer(rs),
      rs->lpVtbl->GetBufferSize(rs), &IID_ID3D12RootSignature,
      (void **)&ctx->root_signature);
  if (FAILED(hr))
    goto done;

  hr = D3DCompile(shader_src, strlen(shader_src), NULL, NULL, NULL, "vs_main",
                  "vs_5_0", 0, 0, &vs, &err);
  if (FAILED(hr))
    goto done;
  hr = D3DCompile(shader_src, strlen(shader_src), NULL, NULL, NULL, "ps_main",
                  "ps_5_0", 0, 0, &ps, &err);
  if (FAILED(hr))
    goto done;

  memset(&pso_desc, 0, sizeof(pso_desc));
  pso_desc.pRootSignature = ctx->root_signature;
  pso_desc.VS.pShaderBytecode = vs->lpVtbl->GetBufferPointer(vs);
  pso_desc.VS.BytecodeLength = vs->lpVtbl->GetBufferSize(vs);
  pso_desc.PS.pShaderBytecode = ps->lpVtbl->GetBufferPointer(ps);
  pso_desc.PS.BytecodeLength = ps->lpVtbl->GetBufferSize(ps);
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso_desc.DepthStencilState.DepthEnable = FALSE;
  pso_desc.DepthStencilState.StencilEnable = FALSE;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;
  hr = ctx->device->lpVtbl->CreateGraphicsPipelineState(
      ctx->device, &pso_desc, &IID_ID3D12PipelineState, (void **)&ctx->pso);

done:
  if (err)
    err->lpVtbl->Release(err);
  if (rs)
    rs->lpVtbl->Release(rs);
  if (vs)
    vs->lpVtbl->Release(vs);
  if (ps)
    ps->lpVtbl->Release(ps);
  return SUCCEEDED(hr) ? 0 : -1;
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
  D3D12_DESCRIPTOR_HEAP_DESC shd;
  DXGI_SWAP_CHAIN_DESC1 sd;
  IDXGIFactory4 *factory = NULL;
  IDXGISwapChain1 *swapchain1 = NULL;
  HRESULT hr;
  memset(ctx, 0, sizeof(*ctx));
  memset(&qd, 0, sizeof(qd));
  memset(&hd, 0, sizeof(hd));
  memset(&shd, 0, sizeof(shd));
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

  shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  shd.NumDescriptors = MAX_FRAMES;
  shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  hr = ctx->device->lpVtbl->CreateDescriptorHeap(
      ctx->device, &shd, &IID_ID3D12DescriptorHeap, (void **)&ctx->srv_heap);
  if (FAILED(hr))
    return -1;
  ctx->srv_size = ctx->device->lpVtbl->GetDescriptorHandleIncrementSize(
      ctx->device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (create_texture_pipeline(ctx) != 0)
    return -1;

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

static D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle(d3d12_context_t *ctx,
                                                  int idx) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle;
  ctx->srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(ctx->srv_heap,
                                                            &handle);
  handle.ptr += (SIZE_T)idx * ctx->srv_size;
  return handle;
}

static D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu_handle(d3d12_context_t *ctx,
                                                  int idx) {
  D3D12_GPU_DESCRIPTOR_HANDLE handle;
  ctx->srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(ctx->srv_heap,
                                                            &handle);
  handle.ptr += (UINT64)idx * ctx->srv_size;
  return handle;
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

static int present_texture_copy(d3d12_context_t *ctx, ID3D12Resource *resource) {
  D3D12_RESOURCE_DESC rd;
  D3D12_RESOURCE_BARRIER barriers[4];
  D3D12_TEXTURE_COPY_LOCATION src;
  D3D12_TEXTURE_COPY_LOCATION dst;
  D3D12_BOX src_box;
  UINT bb_idx = ctx->swapchain->lpVtbl->GetCurrentBackBufferIndex(ctx->swapchain);
  ID3D12CommandList *lists[1];
  UINT copy_w;
  UINT copy_h;
  UINT dst_x;
  UINT dst_y;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(ctx, (int)bb_idx);
  const float clear[4] = {0.02f, 0.02f, 0.025f, 1.0f};

  resource->lpVtbl->GetDesc(resource, &rd);
  copy_w = rd.Width < WIDTH ? (UINT)rd.Width : WIDTH;
  copy_h = rd.Height < HEIGHT ? (UINT)rd.Height : HEIGHT;
  dst_x = (WIDTH - copy_w) / 2u;
  dst_y = (HEIGHT - copy_h) / 2u;

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
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;

  barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[2].Transition.pResource = resource;
  barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  barriers[3] = barriers[0];
  barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

  memset(&src, 0, sizeof(src));
  memset(&dst, 0, sizeof(dst));
  memset(&src_box, 0, sizeof(src_box));
  src.pResource = resource;
  src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  src.SubresourceIndex = 0;
  dst.pResource = ctx->backbuffers[bb_idx];
  dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  dst.SubresourceIndex = 0;
  src_box.left = 0;
  src_box.top = 0;
  src_box.front = 0;
  src_box.right = copy_w;
  src_box.bottom = copy_h;
  src_box.back = 1;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->ClearRenderTargetView(ctx->list, rtv, clear, 0, NULL);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[2]);
  ctx->list->lpVtbl->CopyTextureRegion(ctx->list, &dst, dst_x, dst_y, 0, &src,
                                       &src_box);
  barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
  barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[2]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[3]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    return -1;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  ctx->swapchain->lpVtbl->Present(ctx->swapchain, 1, 0);
  return wait_gpu(ctx);
}

static int present_texture_draw(d3d12_context_t *ctx, ID3D12Resource *resource,
                                int srv_idx, int flip_y) {
  D3D12_RESOURCE_BARRIER barriers[4];
  D3D12_VIEWPORT viewport;
  D3D12_RECT scissor;
  UINT bb_idx = ctx->swapchain->lpVtbl->GetCurrentBackBufferIndex(ctx->swapchain);
  D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_handle(ctx, (int)bb_idx);
  ID3D12DescriptorHeap *heaps[1] = {ctx->srv_heap};
  ID3D12CommandList *lists[1];
  float constants[4] = {(float)flip_y, 0.0f, 0.0f, 0.0f};

  if (FAILED(ctx->allocator->lpVtbl->Reset(ctx->allocator)))
    return -1;
  if (FAILED(ctx->list->lpVtbl->Reset(ctx->list, ctx->allocator, ctx->pso)))
    return -1;

  memset(barriers, 0, sizeof(barriers));
  barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[0].Transition.pResource = ctx->backbuffers[bb_idx];
  barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barriers[1].Transition.pResource = resource;
  barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barriers[2] = barriers[1];
  barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
  barriers[3] = barriers[0];
  barriers[3].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barriers[3].Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;

  viewport.TopLeftX = 0.0f;
  viewport.TopLeftY = 0.0f;
  viewport.Width = (float)WIDTH;
  viewport.Height = (float)HEIGHT;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  scissor.left = 0;
  scissor.top = 0;
  scissor.right = WIDTH;
  scissor.bottom = HEIGHT;

  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[0]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[1]);
  ctx->list->lpVtbl->SetGraphicsRootSignature(ctx->list, ctx->root_signature);
  ctx->list->lpVtbl->SetDescriptorHeaps(ctx->list, 1, heaps);
  ctx->list->lpVtbl->SetGraphicsRootDescriptorTable(
      ctx->list, 0, srv_gpu_handle(ctx, srv_idx));
  ctx->list->lpVtbl->SetGraphicsRoot32BitConstants(ctx->list, 1, 4, constants,
                                                   0);
  ctx->list->lpVtbl->RSSetViewports(ctx->list, 1, &viewport);
  ctx->list->lpVtbl->RSSetScissorRects(ctx->list, 1, &scissor);
  ctx->list->lpVtbl->OMSetRenderTargets(ctx->list, 1, &rtv, FALSE, NULL);
  ctx->list->lpVtbl->IASetPrimitiveTopology(
      ctx->list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->list->lpVtbl->DrawInstanced(ctx->list, 3, 1, 0, 0);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[2]);
  ctx->list->lpVtbl->ResourceBarrier(ctx->list, 1, &barriers[3]);
  if (FAILED(ctx->list->lpVtbl->Close(ctx->list)))
    return -1;

  lists[0] = (ID3D12CommandList *)ctx->list;
  ctx->queue->lpVtbl->ExecuteCommandLists(ctx->queue, 1, lists);
  ctx->swapchain->lpVtbl->Present(ctx->swapchain, 1, 0);
  return wait_gpu(ctx);
}

static int read_center_pixel(d3d12_context_t *ctx, ID3D12Resource *resource,
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
    const size_t x = (size_t)(rd.Width / 2);
    const size_t y = (size_t)(rd.Height / 2);
    const unsigned char *p =
        (const unsigned char *)mapped + y * footprint.Footprint.RowPitch +
        x * 4u;
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
  const char *name = (argc > 1) ? argv[1] : "d3d_interop";

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
  texlink_meta_t meta = texlink_client_meta(client);

  texlink_d3d12_texture_frame_t *texture_frames[MAX_FRAMES] = {0};
  ID3D12Fence *sync_fences[MAX_FRAMES] = {0};
  for (uint32_t i = 0; i < frame_count; i++) {
    texlink_frame_t *source_frame = texlink_client_frame(client, i);
    texture_frames[i] =
        texlink_d3d12_texture_frame_import(&(texlink_d3d12_import_desc_t){
            .version = 1,
            .device = ctx.device,
            .frame = source_frame,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_d3d12_texture_frame_import failed: %s\n",
              texlink_d3d12_last_error_string());
      texlink_client_destroy(client);
      release_context(&ctx);
      glfwTerminate();
      return 1;
    }

    ID3D12Resource *resource =
        texlink_d3d12_texture_frame_resource(texture_frames[i]);
    D3D12_RESOURCE_DESC rd;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc;
    resource->lpVtbl->GetDesc(resource, &rd);
    memset(&srv_desc, 0, sizeof(srv_desc));
    srv_desc.Format = rd.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    ctx.device->lpVtbl->CreateShaderResourceView(
        ctx.device, resource, &srv_desc, srv_cpu_handle(&ctx, (int)i));

    texlink_native_handle_t sync_handle;
    uint64_t sync_value;
    if (texlink_frame_get_sync_native_handle(
            source_frame, TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
            &sync_handle, &sync_value) == 0) {
      HRESULT hr = ctx.device->lpVtbl->OpenSharedHandle(
          ctx.device, (HANDLE)sync_handle.value.ptr, &IID_ID3D12Fence,
          (void **)&sync_fences[i]);
      if (FAILED(hr))
        sync_fences[i] = NULL;
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

    wait_shared_fence(sync_fences[idx], texlink_frame_sync_value(frame));
    unsigned char bgra[4] = {0};
    ID3D12Resource *resource =
        texlink_d3d12_texture_frame_resource(texture_frames[idx]);
    int flip_y = texlink_frame_should_flip_y((texlink_backend_t)meta.backend,
                                             TEXLINK_BACKEND_D3D12);
    if (present_texture_draw(&ctx, resource, idx, flip_y) != 0 &&
        present_texture_copy(&ctx, resource) != 0) {
      read_center_pixel(&ctx, resource, bgra);
      present_color(&ctx, bgra);
    }
    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  for (uint32_t i = 0; i < frame_count; i++)
    texlink_d3d12_texture_frame_destroy(texture_frames[i]);
  for (uint32_t i = 0; i < frame_count; i++) {
    if (sync_fences[i])
      sync_fences[i]->lpVtbl->Release(sync_fences[i]);
  }
  texlink_client_destroy(client);
  release_context(&ctx);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
