#include <GLFW/glfw3.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_core.h>

#include <texlink.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 512
#define HEIGHT 512
#define MAX_IMAGES 2
#define MAX_SC_IMGS 8

typedef struct {
  VkInstance instance;
  VkPhysicalDevice phys;
  VkDevice device;
  VkQueue graphics_queue;
  uint32_t graphics_queue_family;
  VkCommandPool cmd_pool;
  VkCommandBuffer cmd;
  VkFence fence;
  VkPhysicalDeviceMemoryProperties mem_props;
  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkImage sc_images[MAX_SC_IMGS];
  uint32_t sc_image_count;
  VkFormat sc_format;
  VkExtent2D sc_extent;
  VkSemaphore image_available;
  VkSemaphore render_finished;
} VulkanContext;

typedef struct {
  VkImage image;
  VkDeviceMemory memory;
  VkImageLayout current_layout;
} ImportedImage;

static void vk_check(VkResult r, const char *msg) {
  if (r != VK_SUCCESS) {
    fprintf(stderr, "%s failed (%d)\n", msg, r);
    exit(1);
  }
}

static uint32_t find_memory_type(VulkanContext *ctx, uint32_t type_bits,
                                 VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < ctx->mem_props.memoryTypeCount; i++) {
    if ((type_bits & (1u << i)) &&
        (ctx->mem_props.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  }
  return UINT32_MAX;
}

static void create_instance(VulkanContext *ctx) {
  VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "dma consumer",
      .apiVersion = VK_API_VERSION_1_2,
  };
  uint32_t ext_count = 0;
  const char **exts = glfwGetRequiredInstanceExtensions(&ext_count);
  VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = ext_count,
      .ppEnabledExtensionNames = exts,
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

  const char *dev_exts[] = {
      VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
      VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
      VK_KHR_SWAPCHAIN_EXTENSION_NAME,
  };

  VkDeviceCreateInfo dci = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &qci,
      .enabledExtensionCount = sizeof(dev_exts) / sizeof(dev_exts[0]),
      .ppEnabledExtensionNames = dev_exts,
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
    fprintf(stderr, "No surface formats available\n");
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

  ctx->sc_format = chosen.format;
  ctx->sc_extent = (caps.currentExtent.width != UINT32_MAX)
                       ? caps.currentExtent
                       : (VkExtent2D){WIDTH, HEIGHT};
  free(formats);

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
      /* TRANSFER_DST_BIT: blit destination from shared image */
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

/*
 * Import a DMA-BUF fd into Vulkan as a LINEAR image for use as blit source.
 * Vulkan may consume the imported fd, so request an owned duplicate from
 * texlink and keep the original frame handle valid for later use.
 */
static void import_dma_buf_image(VulkanContext *ctx, ImportedImage *img,
                                 texlink_frame_t *frame,
                                 const texlink_meta_t *meta) {
  texlink_native_handle_t handle;
  if (texlink_frame_dup_native_handle(frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
                                      &handle) != 0) {
    fprintf(stderr, "texlink_frame_dup_native_handle failed\n");
    exit(1);
  }

  img->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkExternalMemoryImageCreateInfo ext_img = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
  };
  VkImageCreateInfo ici = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext_img,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = VK_FORMAT_B8G8R8A8_UNORM,
      .extent = {meta->width, meta->height, 1},
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_LINEAR,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
  };
  vk_check(vkCreateImage(ctx->device, &ici, NULL, &img->image),
           "vkCreateImage");

  VkMemoryRequirements mem_req;
  vkGetImageMemoryRequirements(ctx->device, img->image, &mem_req);

  VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = img->image,
  };
  VkImportMemoryFdInfoKHR import_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = handle.value.fd,
  };
  VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import_info,
      .allocationSize = mem_req.size,
      .memoryTypeIndex = find_memory_type(ctx, mem_req.memoryTypeBits, 0),
  };
  if (mai.memoryTypeIndex == UINT32_MAX) {
    fprintf(stderr, "No suitable memory type for DMA-BUF import\n");
    exit(1);
  }
  vk_check(vkAllocateMemory(ctx->device, &mai, NULL, &img->memory),
           "vkAllocateMemory");
  vk_check(vkBindImageMemory(ctx->device, img->image, img->memory, 0),
           "vkBindImageMemory");
}

/*
 * Blit the shared image into the next swapchain image and present.
 *
 * Shared image layout transitions:
 *   current_layout → TRANSFER_SRC_OPTIMAL (blit) → GENERAL
 *
 * The producer leaves shared images in GENERAL; we transition from there.
 * On the very first frame current_layout is UNDEFINED (discard is fine because
 * texlink_client_acquire_frame has already confirmed the producer wrote new
 * data).
 */
static void display_frame(VulkanContext *ctx, ImportedImage *img,
                          const texlink_meta_t *meta) {
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

  /* Transition shared image → TRANSFER_SRC_OPTIMAL */
  VkImageMemoryBarrier shared_to_src = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = img->current_layout,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img->image,
      .subresourceRange = full,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
  };

  /* Transition swapchain image UNDEFINED → TRANSFER_DST_OPTIMAL */
  VkImageMemoryBarrier sc_to_dst = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->sc_images[sc_idx],
      .subresourceRange = full,
      .srcAccessMask = 0,
      .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
  };

  VkImageMemoryBarrier pre_barriers[] = {shared_to_src, sc_to_dst};
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2,
                       pre_barriers);

  int32_t sw = (int32_t)ctx->sc_extent.width;
  int32_t sh = (int32_t)ctx->sc_extent.height;
  /* EGL/OpenGL stores rows bottom-up; flip Y so the image appears correct. */
  int flip_y = (meta->backend == TEXLINK_BACKEND_EGL);
  VkImageBlit region = {
      .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .srcOffsets = {{0, flip_y ? (int32_t)meta->height : 0, 0},
                     {(int32_t)meta->width, flip_y ? 0 : (int32_t)meta->height,
                      1}},
      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
      .dstOffsets = {{0, 0, 0}, {sw, sh, 1}},
  };
  vkCmdBlitImage(ctx->cmd, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 ctx->sc_images[sc_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 1, &region, VK_FILTER_NEAREST);

  /* Transition swapchain image → PRESENT_SRC_KHR */
  VkImageMemoryBarrier sc_to_present = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = ctx->sc_images[sc_idx],
      .subresourceRange = full,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = 0,
  };

  /* Transition shared image → GENERAL for next producer write */
  VkImageMemoryBarrier shared_to_general = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
      .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      .newLayout = VK_IMAGE_LAYOUT_GENERAL,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .image = img->image,
      .subresourceRange = full,
      .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
      .dstAccessMask = 0,
  };

  VkImageMemoryBarrier post_barriers[] = {sc_to_present, shared_to_general};
  vkCmdPipelineBarrier(ctx->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0,
                       NULL, 2, post_barriers);

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

  img->current_layout = VK_IMAGE_LAYOUT_GENERAL;

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

int main(void) {
  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "vulkan tex consumer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    return 1;
  }

  VulkanContext vk = {0};
  create_instance(&vk);

  /* Surface must be created before device (needed for queue family selection)
   */
  vk_check(glfwCreateWindowSurface(vk.instance, window, NULL, &vk.surface),
           "glfwCreateWindowSurface");

  create_device(&vk);
  create_swapchain(&vk);

  printf("Connecting to 'texshare'...\n");
  texlink_client_desc_t desc = {
      .version = 1,
      .name = "texshare",
      .backend = TEXLINK_BACKEND_VULKAN,
      .timeout_ms = 5000,
  };
  texlink_client_t *client = texlink_client_create(&desc);
  if (!client || texlink_client_connect(client) < 0) {
    fprintf(stderr, "texlink_client_connect failed\n");
    return 1;
  }
  printf("Connected.\n");

  texlink_meta_t meta = texlink_client_meta(client);

  ImportedImage images[MAX_IMAGES];
  memset(images, 0, sizeof(images));

  uint32_t frame_count = texlink_client_frame_count(client);
  if (frame_count > MAX_IMAGES)
    frame_count = MAX_IMAGES;
  for (uint32_t i = 0; i < frame_count; i++) {
    texlink_frame_t *frame = texlink_client_frame(client, i);
    if (!frame)
      break;
    import_dma_buf_image(&vk, &images[i], frame, &meta);
  }

  while (!glfwWindowShouldClose(window)) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed\n");
      break;
    }
    int idx = texlink_frame_index(frame);

    display_frame(&vk, &images[idx], &meta);

    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  texlink_client_destroy(client);
  vkDeviceWaitIdle(vk.device);

  for (int i = 0; i < MAX_IMAGES; i++) {
    if (images[i].image)
      vkDestroyImage(vk.device, images[i].image, NULL);
    if (images[i].memory)
      vkFreeMemory(vk.device, images[i].memory, NULL);
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
