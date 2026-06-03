#define GLFW_EXPOSE_NATIVE_COCOA
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <texlink_metal.h>

#include <math.h>
#include <stdio.h>

#define WIDTH 512
#define HEIGHT 512

static CAMetalLayer *create_layer(GLFWwindow *window, id<MTLDevice> device) {
  NSWindow *ns_window = glfwGetCocoaWindow(window);
  NSView *view = [ns_window contentView];
  [view setWantsLayer:YES];
  CAMetalLayer *layer = [CAMetalLayer layer];
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.framebufferOnly = NO;
  [view setLayer:layer];
  return layer;
}

static void update_drawable_size(GLFWwindow *window, CAMetalLayer *layer) {
  int w = 0, h = 0;
  glfwGetFramebufferSize(window, &w, &h);
  layer.drawableSize = CGSizeMake((CGFloat)w, (CGFloat)h);
}

int main(int argc, char **argv) {
  @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *name = (argc > 1) ? argv[1] : "texlink";

    if (!glfwInit())
      return 1;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    GLFWwindow *window =
        glfwCreateWindow(WIDTH, HEIGHT, "metal tex producer", NULL, NULL);
    if (!window)
      return 1;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      fprintf(stderr, "MTLCreateSystemDefaultDevice failed\n");
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    CAMetalLayer *layer = create_layer(window, device);

    texlink_metal_texture_frame_t *texture_frames[2] = {
        texlink_metal_texture_frame_create(
            &(texlink_metal_texture_frame_desc_t){
                .device = device,
                .width = WIDTH,
                .height = HEIGHT,
                .format = TEXLINK_FRAME_FORMAT_ARGB8888,
            }),
        texlink_metal_texture_frame_create(
            &(texlink_metal_texture_frame_desc_t){
                .device = device,
                .width = WIDTH,
                .height = HEIGHT,
                .format = TEXLINK_FRAME_FORMAT_ARGB8888,
            }),
    };
    texlink_frame_t *frames[2] = {
        texlink_metal_texture_frame_frame(texture_frames[0]),
        texlink_metal_texture_frame_frame(texture_frames[1]),
    };
    if (!frames[0] || !frames[1]) {
      fprintf(stderr, "texlink_metal_texture_frame_create failed: %s\n",
              texlink_metal_last_error_string());
      return 1;
    }

    texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
        .name = name,
        .backend_type = TEXLINK_BACKEND_METAL,
        .frames = frames,
        .frame_count = 2,
    });
    if (!server || texlink_server_start(server) < 0) {
      fprintf(stderr, "texlink_server_start failed\n");
      return 1;
    }
    printf("Serving Metal texture session '%s'...\n", name);

    uint64_t counter = 0;
    while (!glfwWindowShouldClose(window)) {
      @autoreleasepool {
        texlink_server_poll(server);
        texlink_frame_t *frame = texlink_server_begin_frame(server);
        if (!frame) {
          glfwPollEvents();
          continue;
        }

        int idx = texlink_frame_index(frame);
        float t = (float)counter * 0.035f;
        double r = 0.5 + 0.5 * sin(t);
        double g = 0.5 + 0.5 * sin(t + 2.094395);
        double b = 0.5 + 0.5 * sin(t + 4.188790);
        id<MTLTexture> shared =
            (id<MTLTexture>)texlink_metal_texture_frame_texture(texture_frames[idx]);

        MTLRenderPassDescriptor *rp =
            [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = shared;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(r, g, b, 1.0);

        update_drawable_size(window, layer);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> enc =
            [cb renderCommandEncoderWithDescriptor:rp];
        [enc endEncoding];
        if (drawable) {
          id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
          [blit copyFromTexture:shared
                    sourceSlice:0
                    sourceLevel:0
                   sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(WIDTH, HEIGHT, 1)
                      toTexture:drawable.texture
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
          [blit endEncoding];
          [cb presentDrawable:drawable];
        }
        [cb commit];
        [cb waitUntilCompleted];

        counter++;
        texlink_server_end_frame(server, frame);
        glfwPollEvents();
      }
    }

    texlink_server_destroy(server);
    texlink_metal_texture_frame_destroy(texture_frames[0]);
    texlink_metal_texture_frame_destroy(texture_frames[1]);
    [queue release];
    [device release];
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return 0;
}
