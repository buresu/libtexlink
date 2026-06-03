#define GLFW_EXPOSE_NATIVE_COCOA
#import <Cocoa/Cocoa.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <texlink_metal.h>

#include <stdio.h>
#include <string.h>

#define WIDTH 512
#define HEIGHT 512
#define MAX_SAMPLE_FRAMES 3

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

static id<MTLRenderPipelineState> create_pipeline(id<MTLDevice> device) {
  NSString *source =
      @"#include <metal_stdlib>\n"
       "using namespace metal;\n"
       "struct VSOut { float4 position [[position]]; float2 uv; };\n"
       "vertex VSOut texlink_vertex(uint vid [[vertex_id]],\n"
       "                            const device float4 *v [[buffer(0)]]) {\n"
       "  VSOut out;\n"
       "  out.position = float4(v[vid].xy, 0.0, 1.0);\n"
       "  out.uv = v[vid].zw;\n"
       "  return out;\n"
       "}\n"
       "fragment float4 texlink_fragment(VSOut in [[stage_in]],\n"
       "                                texture2d<float> tex [[texture(0)]]) {\n"
       "  constexpr sampler s(address::clamp_to_edge, filter::linear);\n"
       "  return tex.sample(s, in.uv);\n"
       "}\n";
  NSError *error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:source
                                                options:nil
                                                  error:&error];
  if (!library) {
    fprintf(stderr, "newLibraryWithSource failed: %s\n",
            [[error localizedDescription] UTF8String]);
    return nil;
  }

  MTLRenderPipelineDescriptor *desc =
      [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
  desc.vertexFunction = [library newFunctionWithName:@"texlink_vertex"];
  desc.fragmentFunction = [library newFunctionWithName:@"texlink_fragment"];
  desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;

  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:desc error:&error];
  [desc.vertexFunction release];
  [desc.fragmentFunction release];
  [library release];
  if (!pipeline) {
    fprintf(stderr, "newRenderPipelineStateWithDescriptor failed: %s\n",
            [[error localizedDescription] UTF8String]);
  }
  return pipeline;
}

static void fill_quad_vertices(float *dst, int flip_y) {
  const float normal[] = {
      -1.0f, -1.0f, 0.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f,
      1.0f,  1.0f,  1.0f, 0.0f, -1.0f, -1.0f, 0.0f, 1.0f,
      1.0f,  1.0f,  1.0f, 0.0f, -1.0f, 1.0f,  0.0f, 0.0f,
  };
  const float flipped[] = {
      -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f,
      1.0f,  1.0f,  1.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f,
      1.0f,  1.0f,  1.0f, 1.0f, -1.0f, 1.0f,  0.0f, 1.0f,
  };
  memcpy(dst, flip_y ? flipped : normal, sizeof(normal));
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
        glfwCreateWindow(WIDTH, HEIGHT, "metal tex consumer", NULL, NULL);
    if (!window)
      return 1;

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      fprintf(stderr, "MTLCreateSystemDefaultDevice failed\n");
      return 1;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> pixel =
        [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    id<MTLRenderPipelineState> pipeline = create_pipeline(device);
    id<MTLBuffer> quad_buffer =
        [device newBufferWithLength:sizeof(float) * 4 * 6
                            options:MTLResourceStorageModeShared];
    if (!pipeline || !quad_buffer) {
      fprintf(stderr, "Metal display pipeline creation failed\n");
      return 1;
    }
    CAMetalLayer *layer = create_layer(window, device);

    texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
        .name = name,
        .backend_type = TEXLINK_BACKEND_METAL,
        .timeout_ms = 5000,
    });
    if (!client || texlink_client_connect(client) < 0) {
      fprintf(stderr, "texlink_client_connect failed\n");
      return 1;
    }
    texlink_meta_t meta = texlink_client_meta(client);
    int frame_count = texlink_client_frame_count(client);
    if (frame_count > MAX_SAMPLE_FRAMES)
      frame_count = MAX_SAMPLE_FRAMES;

    texlink_metal_texture_frame_t *texture_frames[MAX_SAMPLE_FRAMES] = {0};
    for (int i = 0; i < frame_count; i++) {
      texture_frames[i] = texlink_metal_texture_frame_import(
          &(texlink_metal_import_desc_t){
              .device = device,
              .frame = texlink_client_frame(client, i),
          });
      if (!texture_frames[i]) {
        fprintf(stderr, "texlink_metal_texture_frame_import failed: %s\n",
                texlink_metal_last_error_string());
        return 1;
      }
    }
    printf("Connected Metal consumer to '%s' (%ux%u).\n", name, meta.width,
           meta.height);
    fill_quad_vertices((float *)[quad_buffer contents],
                       texlink_frame_should_flip_y(
                           (texlink_backend_t)meta.backend_type,
                           TEXLINK_BACKEND_METAL));

    while (!glfwWindowShouldClose(window)) {
      @autoreleasepool {
        texlink_frame_t *frame = texlink_client_acquire_frame(client);
        if (!frame) {
          fprintf(stderr, "Acquire failed (producer disconnected?)\n");
          break;
        }
        int idx = texlink_frame_index(frame);
        id<MTLTexture> tex =
            (id<MTLTexture>)texlink_metal_texture_frame_texture(texture_frames[idx]);

        update_drawable_size(window, layer);
        id<CAMetalDrawable> drawable = [layer nextDrawable];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (drawable) {
          MTLRenderPassDescriptor *rp =
              [MTLRenderPassDescriptor renderPassDescriptor];
          rp.colorAttachments[0].texture = drawable.texture;
          rp.colorAttachments[0].loadAction = MTLLoadActionClear;
          rp.colorAttachments[0].storeAction = MTLStoreActionStore;
          rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
          id<MTLRenderCommandEncoder> enc =
              [cb renderCommandEncoderWithDescriptor:rp];
          [enc setRenderPipelineState:pipeline];
          [enc setVertexBuffer:quad_buffer offset:0 atIndex:0];
          [enc setFragmentTexture:tex atIndex:0];
          [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
          [enc endEncoding];
        }
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromTexture:tex
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(meta.width / 2, meta.height / 2, 0)
                   sourceSize:MTLSizeMake(1, 1, 1)
                     toBuffer:pixel
            destinationOffset:0
       destinationBytesPerRow:4
     destinationBytesPerImage:4];
        [blit endEncoding];
        if (drawable)
          [cb presentDrawable:drawable];
        [cb commit];
        [cb waitUntilCompleted];

        unsigned char *bgra = (unsigned char *)[pixel contents];
        texlink_meta_t cur = texlink_client_meta(client);
        printf("frame=%llu slot=%d bgra=(%u %u %u %u)\n",
               (unsigned long long)cur.frame_id, idx, bgra[0], bgra[1],
               bgra[2], bgra[3]);

        texlink_client_release_frame(client, frame);
        glfwPollEvents();
      }
    }

    for (int i = 0; i < frame_count; i++)
      texlink_metal_texture_frame_destroy(texture_frames[i]);
    texlink_client_destroy(client);
    [quad_buffer release];
    [pipeline release];
    [pixel release];
    [queue release];
    [device release];
    glfwDestroyWindow(window);
    glfwTerminate();
  }
  return 0;
}
