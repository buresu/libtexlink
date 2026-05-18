#ifndef _WIN32
#define _POSIX_C_SOURCE 199309L
#endif
#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include <texlink_vulkan.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define WIDTH 512
#define HEIGHT 512
#define MAX_SC_IMGS 8

static void sleep_until_next_frame(double *last_time, double interval_sec) {
  double now = glfwGetTime();
  double wait = *last_time + interval_sec - now;
  if (wait > 0.0) {
#ifdef _WIN32
    Sleep((DWORD)(wait * 1000.0));
#else
    struct timespec ts = {
        .tv_sec = (time_t)wait,
        .tv_nsec = (long)((wait - (time_t)wait) * 1e9),
    };
    nanosleep(&ts, NULL);
#endif
  }
  *last_time = glfwGetTime();
}

typedef struct {
  VkInstance instance;
  VkPhysicalDevice phys;
  VkDevice device;
  uint32_t graphics_queue_family;
  VkQueue graphics_queue;
  VkCommandPool cmd_pool;
  VkCommandBuffer cmd;
  VkFence fence;
  VkPhysicalDeviceMemoryProperties mem_props;
  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkImage sc_images[MAX_SC_IMGS];
  uint32_t sc_image_count;
  VkExtent2D sc_extent;
  VkSemaphore image_available;
  VkSemaphore render_finished;
} VulkanContext;

typedef struct {
  texlink_vk_image_t *image;
  texlink_frame_t *frame;
#ifdef _WIN32
  ID3D11Device *d3d11_device;
  ID3D11DeviceContext *d3d11_context;
  ID3D11Texture2D *d3d11_texture;
  HANDLE shared_handle;
#endif
} SharedImage;

#ifdef _WIN32
static void destroy_shared_image(SharedImage *img) {
  texlink_vk_image_destroy(img->image);
  if (img->frame)
    texlink_frame_destroy(img->frame);
  if (img->shared_handle)
    CloseHandle(img->shared_handle);
  if (img->d3d11_texture)
    img->d3d11_texture->lpVtbl->Release(img->d3d11_texture);
  if (img->d3d11_context)
    img->d3d11_context->lpVtbl->Release(img->d3d11_context);
  if (img->d3d11_device)
    img->d3d11_device->lpVtbl->Release(img->d3d11_device);
  memset(img, 0, sizeof(*img));
}

static int create_d3d11_shared_frame(SharedImage *img) {
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

  hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels,
                         (UINT)(sizeof(levels) / sizeof(levels[0])),
                         D3D11_SDK_VERSION, &img->d3d11_device, &selected,
                         &img->d3d11_context);
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
  hr = img->d3d11_device->lpVtbl->CreateTexture2D(img->d3d11_device, &td, NULL,
                                                  &img->d3d11_texture);
  if (FAILED(hr)) {
    td.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
    hr = img->d3d11_device->lpVtbl->CreateTexture2D(
        img->d3d11_device, &td, NULL, &img->d3d11_texture);
    if (FAILED(hr))
      return -1;
  }

  hr = img->d3d11_texture->lpVtbl->QueryInterface(
      img->d3d11_texture, &IID_IDXGIResource1, (void **)&resource);
  if (FAILED(hr) || !resource)
    return -1;
  hr = resource->lpVtbl->CreateSharedHandle(resource, NULL, GENERIC_ALL, NULL,
                                            &img->shared_handle);
  resource->lpVtbl->Release(resource);
  if (FAILED(hr) || !img->shared_handle)
    return -1;

  texlink_native_handle_t handle = {
      .handle_type = TEXLINK_NATIVE_HANDLE_D3D12_SHARED_HANDLE,
      .owned = 0,
      .value.ptr = img->shared_handle,
  };
  img->frame = texlink_frame_create_from_native_handle(
      &(texlink_frame_native_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_TEXTURE_2D,
          .width = WIDTH,
          .height = HEIGHT,
          .depth = 1,
          .format = TEXLINK_FRAME_FORMAT_ARGB8888,
          .stride = WIDTH * 4,
          .size = WIDTH * HEIGHT * 4,
          .backend_type = TEXLINK_BACKEND_VULKAN,
          .handle = handle,
      });
  return img->frame ? 0 : -1;
}
#else
static void destroy_shared_image(SharedImage *img) {
  texlink_vk_image_destroy(img->image);
  memset(img, 0, sizeof(*img));
}
#endif

static void vk_check(VkResult r, const char *msg) {
  if (r != VK_SUCCESS) {
    fprintf(stderr, "%s failed (%d)\n", msg, r);
    exit(1);
  }
}

static void create_instance(VulkanContext *ctx) {
  VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "dma producer",
      .apiVersion = VK_API_VERSION_1_2,
  };
  uint32_t glfw_ext_count = 0;
  const char **glfw_exts = glfwGetRequiredInstanceExtensions(&glfw_ext_count);
  VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = glfw_ext_count,
      .ppEnabledExtensionNames = glfw_exts,
  };
  vk_check(vkCreateInstance(&ci, NULL, &ctx->instance), "vkCreateInstance");
}

static void create_device(VulkanContext *ctx) {
  uint32_t dev_count = 0;
  vkEnumeratePhysicalDevices(ctx->instance, &dev_count, NULL);
  if (dev_count == 0) {
    fprintf(stderr, "No Vulkan devices found\n");
    exit(1);
  }
  VkPhysicalDevice devs[16];
  if (dev_count > 16)
    dev_count = 16;
  vkEnumeratePhysicalDevices(ctx->instance, &dev_count, devs);
  ctx->phys = devs[0];
  vkGetPhysicalDeviceMemoryProperties(ctx->phys, &ctx->mem_props);

  uint32_t qcount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx->phys, &qcount, NULL);
  VkQueueFamilyProperties qprops[32];
  if (qcount > 32)
    qcount = 32;
  vkGetPhysicalDeviceQueueFamilyProperties(ctx->phys, &qcount, qprops);
  for (uint32_t i = 0; i < qcount; i++) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx->phys, i, ctx->surface, &present);
    if ((qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
      ctx->graphics_queue_family = i;
      break;
    }
  }

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueFamilyIndex = ctx->graphics_queue_family,
      .queueCount = 1,
      .pQueuePriorities = &priority,
  };

  const char *exts[] = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
#ifdef _WIN32
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
#else
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
#endif
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = sizeof(exts) / sizeof(exts[0]),
      .ppEnabledExtensionNames = exts,
  };
  vk_check(vkCreateDevice(ctx->phys, &dci, NULL, &ctx->device),
           "vkCreateDevice");
  vkGetDeviceQueue(ctx->device, ctx->graphics_queue_family, 0,
                   &ctx->graphics_queue);

  VkCommandPoolCreateInfo pool_ci = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .queueFamilyIndex = ctx->graphics_queue_family,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
  };
  vk_check(vkCreateCommandPool(ctx->device, &pool_ci, NULL, &ctx->cmd_pool),
           "vkCreateCommandPool");

  VkCommandBufferAllocateInfo cmd_ai = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = ctx->cmd_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
  };
  vk_check(vkAllocateCommandBuffers(ctx->device, &cmd_ai, &ctx->cmd),
           "vkAllocateCommandBuffers");

  VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  vk_check(vkCreateFence(ctx->device, &fci, NULL, &ctx->fence),
           "vkCreateFence");

  VkSemaphoreCreateInfo sci = {.sType =
                                   VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  vk_check(vkCreateSemaphore(ctx->device, &sci, NULL, &ctx->image_available),
           "image_available semaphore");
  vk_check(vkCreateSemaphore(ctx->device, &sci, NULL, &ctx->render_finished),
           "render_finished semaphore");
}

static void create_swapchain(VulkanContext *ctx) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->phys, ctx->surface, &caps);

  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, ctx->surface, &fmt_count,
                                       NULL);
  if (fmt_count == 0) {
    fprintf(stderr, "No surface formats\n");
    exit(1);
  }
  VkSurfaceFormatKHR *formats = malloc(fmt_count * sizeof(*formats));
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->phys, ctx->surface, &fmt_count,
                                       formats);

  VkSurfaceFormatKHR chosen = formats[0];
  for (uint32_t i = 0; i < fmt_count; i++) {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
        formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = formats[i];
      break;
    }
  }
  free(formats);

  ctx->sc_extent = (caps.currentExtent.width != UINT32_MAX)
                       ? caps.currentExtent
                       : (VkExtent2D){WIDTH, HEIGHT};

  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    image_count = caps.maxImageCount;

  if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    fprintf(stderr, "Swapchain TRANSFER_DST not supported\n");
    exit(1);
  }

  VkSwapchainCreateInfoKHR swapchain_ci = {
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = ctx->surface,
      .minImageCount = image_count,
      .imageFormat = chosen.format,
      .imageColorSpace = chosen.colorSpace,
      .imageExtent = ctx->sc_extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = caps.currentTransform,
      .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      .presentMode = VK_PRESENT_MODE_FIFO_KHR,
      .clipped = VK_TRUE,
  };
  vk_check(
      vkCreateSwapchainKHR(ctx->device, &swapchain_ci, NULL, &ctx->swapchain),
      "vkCreateSwapchainKHR");

  ctx->sc_image_count = MAX_SC_IMGS;
  vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &ctx->sc_image_count,
                          ctx->sc_images);
}

static void submit_and_wait(VulkanContext *ctx) {
  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->cmd,
  };
  vkQueueSubmit(ctx->graphics_queue, 1, &submit, ctx->fence);
  vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
  vkResetFences(ctx->device, 1, &ctx->fence);
  vkResetCommandBuffer(ctx->cmd, 0);
}

/*
 * Clear the shared image with an animated solid color.
 * Transitions: UNDEFINED -> TRANSFER_DST_OPTIMAL (clear) -> GENERAL
 */
static void render_frame(VulkanContext *ctx, SharedImage *img, float t) {
  VkImage image = texlink_vk_image_handle(img->image);
  float r = 0.5f + 0.5f * sinf(t);
  float g = 0.5f + 0.5f * sinf(t + 2.094f);
  float b = 0.5f + 0.5f * sinf(t + 4.189f);

  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(ctx->cmd, &bi);

  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkImageMemoryBarrier to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = range,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
  };
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &to_dst);

  VkClearColorValue color = {.float32 = {r, g, b, 1.0f}};
  vkCmdClearColorImage(ctx->cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &color, 1, &range);

  VkImageMemoryBarrier to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = image,
      .subresourceRange = range,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = 0,
  };
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                       NULL, 1, &to_general);

  vkEndCommandBuffer(ctx->cmd);
  submit_and_wait(ctx);
}

/*
 * Blit the just-rendered shared image into the next swapchain image for
 * preview. The shared image is in GENERAL layout after render_frame.
 */
static void preview_frame(VulkanContext *ctx, VkImage src_image) {
  uint32_t sc_idx = 0;
  VkResult res =
      vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX,
                            ctx->image_available, VK_NULL_HANDLE, &sc_idx);
  if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)
    return;
  vk_check(res, "vkAcquireNextImageKHR");

  VkCommandBufferBeginInfo bi = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(ctx->cmd, &bi);

  VkImageSubresourceRange full = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  VkImageMemoryBarrier pre[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = src_image,
          .subresourceRange = full,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      },
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = ctx->sc_images[sc_idx],
          .subresourceRange = full,
          .srcAccessMask = 0,
          .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      },
  };
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                       pre);

  int32_t sw = (int32_t)ctx->sc_extent.width;
  int32_t sh = (int32_t)ctx->sc_extent.height;
  VkImageBlit region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffsets = {{0, 0, 0}, {WIDTH, HEIGHT, 1}},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffsets = {{0, 0, 0}, {sw, sh, 1}},
  };
  vkCmdBlitImage(ctx->cmd, src_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 ctx->sc_images[sc_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region, VK_FILTER_NEAREST);

  VkImageMemoryBarrier post[2] = {
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = ctx->sc_images[sc_idx],
          .subresourceRange = full,
          .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
          .dstAccessMask = 0,
      },
      {
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = src_image,
          .subresourceRange = full,
          .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
          .dstAccessMask = 0,
      },
  };
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                       NULL, 2, post);

  vkEndCommandBuffer(ctx->cmd);

  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &ctx->image_available,
      .pWaitDstStageMask = &wait_stage,
      .commandBufferCount = 1,
      .pCommandBuffers = &ctx->cmd,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &ctx->render_finished,
  };
  vkQueueSubmit(ctx->graphics_queue, 1, &submit, ctx->fence);
  vkWaitForFences(ctx->device, 1, &ctx->fence, VK_TRUE, UINT64_MAX);
  vkResetFences(ctx->device, 1, &ctx->fence);
  vkResetCommandBuffer(ctx->cmd, 0);

  VkPresentInfoKHR present = {
      .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
      .waitSemaphoreCount = 1,
      .pWaitSemaphores = &ctx->render_finished,
      .swapchainCount = 1,
      .pSwapchains = &ctx->swapchain,
      .pImageIndices = &sc_idx,
  };
  vkQueuePresentKHR(ctx->graphics_queue, &present);
}

int main(int argc, char **argv) {
  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "vulkan tex producer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    return 1;
  }

  VulkanContext vk = {0};
  create_instance(&vk);

  vk_check(glfwCreateWindowSurface(vk.instance, window, NULL, &vk.surface),
           "glfwCreateWindowSurface");

  create_device(&vk);
  create_swapchain(&vk);

  SharedImage images[2];
  memset(images, 0, sizeof(images));
  texlink_frame_t *frames[2] = {0};
  for (int i = 0; i < 2; i++) {
#ifdef _WIN32
    if (create_d3d11_shared_frame(&images[i]) != 0) {
      fprintf(stderr, "D3D11 shared texture creation failed\n");
      return 1;
    }
    images[i].image = texlink_vk_image_import(&(texlink_vk_import_desc_t){
        .device = vk.device,
        .memory_properties = vk.mem_props,
        .frame = images[i].frame,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
    });
#else
    images[i].image =
        texlink_vk_image_frame_create(&(texlink_vk_image_frame_desc_t){
            .device = vk.device,
            .memory_properties = vk.mem_props,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        });
    images[i].frame = texlink_vk_image_frame_frame(images[i].image);
#endif
    frames[i] = images[i].frame;
  }
  if (!frames[0] || !frames[1]) {
    fprintf(stderr, "texlink_vk_image_frame_create failed\n");
    return 1;
  }

  const char *name = (argc > 1) ? argv[1] : "texlink";
  printf("Serving '%s'...\n", name);
  texlink_server_desc_t desc = {
      .name = name,
      .backend_type = TEXLINK_BACKEND_VULKAN,
      .frames = frames,
      .frame_count = 2,
  };
  texlink_server_t *server = texlink_server_create(&desc);
  if (!server || texlink_server_start(server) < 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    return 1;
  }
  printf("Rendering...\n");

  double last_frame = glfwGetTime();

  while (!glfwWindowShouldClose(window)) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;
    int idx = texlink_frame_index(frame);

    render_frame(&vk, &images[idx], (float)glfwGetTime());
    preview_frame(&vk, texlink_vk_image_handle(images[idx].image));

    texlink_server_end_frame(server, frame);
    glfwPollEvents();
    sleep_until_next_frame(&last_frame, 1.0 / 60.0);
  }

  texlink_server_destroy(server);

  vkDeviceWaitIdle(vk.device);

  for (int i = 0; i < 2; i++) {
    destroy_shared_image(&images[i]);
  }

  vkDestroySemaphore(vk.device, vk.image_available, NULL);
  vkDestroySemaphore(vk.device, vk.render_finished, NULL);
  vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
  vkDestroyFence(vk.device, vk.fence, NULL);
  vkDestroyCommandPool(vk.device, vk.cmd_pool, NULL);
  vkDestroyDevice(vk.device, NULL);
  vkDestroySurfaceKHR(vk.instance, vk.surface, NULL);
  vkDestroyInstance(vk.instance, NULL);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
