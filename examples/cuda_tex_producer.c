/*
 * CUDA texture producer example
 *
 * Usage: cuda_tex_producer [session_name]
 *        Default name: "texlink"
 *
 * CUDA does not export DMA-BUF memory here. EGL creates the shareable
 * texture-backed DMA-BUF, CUDA imports it as an EGLImage and writes pixels.
 */
#define _POSIX_C_SOURCE 199309L
#define GLFW_INCLUDE_NONE
#include <EGL/egl.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <cuda.h>
#include <cudaEGL.h>
#include <nvrtc.h>

#include <texlink_egl.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIDTH 512
#define HEIGHT 512
#define FRAME_COUNT 2

typedef struct {
  texlink_egl_texture_frame_t *texture_frame;
  texlink_egl_image_t *egl_image;
  CUgraphicsResource cuda_resource;
  CUeglFrame cuda_frame;
  CUsurfObject surface;
} SharedCudaTexture;

static const char *kernel_src =
    "#include <cuda_runtime.h>\n"
    "extern \"C\" __global__ void fill_tex_pitch(unsigned char *data, "
    "int width, int height, int pitch, unsigned int frame) {\n"
    "  int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
    "  int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
    "  if (x >= width || y >= height) return;\n"
    "  unsigned char *p = data + y * pitch + x * 4;\n"
    "  unsigned int wave = (x + y + frame * 4u) & 255u;\n"
    "  unsigned int ring = ((x - width / 2) * (x - width / 2) + "
    "(y - height / 2) * (y - height / 2) + frame * 37u) >> 8;\n"
    "  p[0] = (unsigned char)(wave);\n"
    "  p[1] = (unsigned char)((y + frame * 3u) & 255u);\n"
    "  p[2] = (unsigned char)((ring * 9u) & 255u);\n"
    "  p[3] = 255;\n"
    "}\n"
    "extern \"C\" __global__ void fill_tex_surface(cudaSurfaceObject_t surf, "
    "int width, int height, unsigned int frame) {\n"
    "  int x = blockIdx.x * blockDim.x + threadIdx.x;\n"
    "  int y = blockIdx.y * blockDim.y + threadIdx.y;\n"
    "  if (x >= width || y >= height) return;\n"
    "  unsigned int wave = (x + y + frame * 4u) & 255u;\n"
    "  unsigned int ring = ((x - width / 2) * (x - width / 2) + "
    "(y - height / 2) * (y - height / 2) + frame * 37u) >> 8;\n"
    "  uchar4 color = make_uchar4((unsigned char)wave, "
    "(unsigned char)((y + frame * 3u) & 255u), "
    "(unsigned char)((ring * 9u) & 255u), 255);\n"
    "  surf2Dwrite(color, surf, x * 4, y);\n"
    "}\n";

static void sleep_until_next_frame(double *last_time, double interval_sec) {
  double now = glfwGetTime();
  double wait = *last_time + interval_sec - now;
  if (wait > 0.0) {
    struct timespec ts = {
        .tv_sec = (time_t)wait,
        .tv_nsec = (long)((wait - (time_t)wait) * 1e9),
    };
    nanosleep(&ts, NULL);
  }
  *last_time = glfwGetTime();
}

static const char *cu_result_name(CUresult result) {
  const char *name = NULL;
  if (cuGetErrorName(result, &name) == CUDA_SUCCESS && name)
    return name;
  return "CUDA_ERROR_UNKNOWN";
}

static int cuda_check(CUresult result, const char *what) {
  if (result == CUDA_SUCCESS)
    return 0;
  fprintf(stderr, "%s failed: %s (%d)\n", what, cu_result_name(result),
          result);
  return -1;
}

static int nvrtc_check(nvrtcResult result, const char *what) {
  if (result == NVRTC_SUCCESS)
    return 0;
  fprintf(stderr, "%s failed: %s\n", what, nvrtcGetErrorString(result));
  return -1;
}

static int init_cuda(CUcontext *out_context) {
  if (cuda_check(cuInit(0), "cuInit") != 0)
    return -1;

  CUdevice device = 0;
  if (cuda_check(cuDeviceGet(&device, 0), "cuDeviceGet") != 0)
    return -1;

  char name[128] = {0};
  cuDeviceGetName(name, sizeof(name), device);
  printf("CUDA device: %s\n", name);

  return cuda_check(cuCtxCreate(out_context, 0, device), "cuCtxCreate");
}

static int build_kernels(CUmodule *out_module, CUfunction *out_pitch_kernel,
                         CUfunction *out_surface_kernel) {
  nvrtcProgram program = NULL;
  if (nvrtc_check(nvrtcCreateProgram(&program, kernel_src, "fill_tex.cu", 0,
                                     NULL, NULL),
                  "nvrtcCreateProgram") != 0)
    return -1;

  const char *opts[] = {"--gpu-architecture=compute_52",
                        "--std=c++11",
                        "--include-path=/usr/include"};
  nvrtcResult compile_result =
      nvrtcCompileProgram(program, sizeof(opts) / sizeof(opts[0]), opts);

  size_t log_size = 0;
  nvrtcGetProgramLogSize(program, &log_size);
  if (log_size > 1) {
    char *log = malloc(log_size);
    if (log) {
      nvrtcGetProgramLog(program, log);
      fprintf(stderr, "%s", log);
      free(log);
    }
  }
  if (nvrtc_check(compile_result, "nvrtcCompileProgram") != 0) {
    nvrtcDestroyProgram(&program);
    return -1;
  }

  size_t ptx_size = 0;
  if (nvrtc_check(nvrtcGetPTXSize(program, &ptx_size), "nvrtcGetPTXSize") !=
      0) {
    nvrtcDestroyProgram(&program);
    return -1;
  }

  char *ptx = malloc(ptx_size);
  if (!ptx) {
    nvrtcDestroyProgram(&program);
    return -1;
  }
  if (nvrtc_check(nvrtcGetPTX(program, ptx), "nvrtcGetPTX") != 0) {
    free(ptx);
    nvrtcDestroyProgram(&program);
    return -1;
  }
  nvrtcDestroyProgram(&program);

  if (cuda_check(cuModuleLoadData(out_module, ptx), "cuModuleLoadData") != 0) {
    free(ptx);
    return -1;
  }
  free(ptx);

  if (cuda_check(cuModuleGetFunction(out_pitch_kernel, *out_module,
                                     "fill_tex_pitch"),
                 "cuModuleGetFunction(fill_tex_pitch)") != 0)
    return -1;
  return cuda_check(cuModuleGetFunction(out_surface_kernel, *out_module,
                                        "fill_tex_surface"),
                    "cuModuleGetFunction(fill_tex_surface)");
}

static int import_texture(EGLDisplay display,
                          texlink_egl_texture_frame_t *texture_frame,
                          SharedCudaTexture *out_texture) {
  texlink_frame_t *frame = texlink_egl_texture_frame_frame(texture_frame);
  out_texture->texture_frame = texture_frame;
  out_texture->egl_image = texlink_egl_image_import(&(texlink_egl_import_desc_t){
      .display = display,
      .frame = frame,
  });
  if (!out_texture->egl_image) {
    fprintf(stderr, "texlink_egl_image_import failed\n");
    return -1;
  }

  CUresult result = cuGraphicsEGLRegisterImage(
      &out_texture->cuda_resource,
      (EGLImageKHR)texlink_egl_image_handle(out_texture->egl_image),
      CU_GRAPHICS_MAP_RESOURCE_FLAGS_WRITE_DISCARD);
  if (cuda_check(result, "cuGraphicsEGLRegisterImage") != 0)
    return -1;

  result = cuGraphicsResourceGetMappedEglFrame(&out_texture->cuda_frame,
                                               out_texture->cuda_resource, 0, 0);
  if (cuda_check(result, "cuGraphicsResourceGetMappedEglFrame") != 0)
    return -1;
  if (out_texture->cuda_frame.frameType == CU_EGL_FRAME_TYPE_ARRAY) {
    CUDA_RESOURCE_DESC resource_desc;
    memset(&resource_desc, 0, sizeof(resource_desc));
    resource_desc.resType = CU_RESOURCE_TYPE_ARRAY;
    resource_desc.res.array.hArray = out_texture->cuda_frame.frame.pArray[0];
    result = cuSurfObjectCreate(&out_texture->surface, &resource_desc);
    if (cuda_check(result, "cuSurfObjectCreate") != 0)
      return -1;
  }
  return 0;
}

static int fill_texture(CUfunction pitch_kernel, CUfunction surface_kernel,
                        SharedCudaTexture *texture,
                        unsigned int frame_id) {
  int width = (int)texture->cuda_frame.width;
  int height = (int)texture->cuda_frame.height;
  unsigned int gx = (unsigned int)((width + 15) / 16);
  unsigned int gy = (unsigned int)((height + 15) / 16);

  if (texture->cuda_frame.frameType == CU_EGL_FRAME_TYPE_PITCH) {
    unsigned char *data = texture->cuda_frame.frame.pPitch[0];
    int pitch = (int)texture->cuda_frame.pitch;
    void *args[] = {&data, &width, &height, &pitch, &frame_id};
    if (cuda_check(cuLaunchKernel(pitch_kernel, gx, gy, 1, 16, 16, 1, 0, NULL,
                                  args, NULL),
                   "cuLaunchKernel(fill_tex_pitch)") != 0)
      return -1;
  } else if (texture->cuda_frame.frameType == CU_EGL_FRAME_TYPE_ARRAY) {
    CUsurfObject surface = texture->surface;
    void *args[] = {&surface, &width, &height, &frame_id};
    if (cuda_check(cuLaunchKernel(surface_kernel, gx, gy, 1, 16, 16, 1, 0,
                                  NULL, args, NULL),
                   "cuLaunchKernel(fill_tex_surface)") != 0)
      return -1;
  } else {
    fprintf(stderr, "Unsupported CUDA EGL frame type %d\n",
            texture->cuda_frame.frameType);
    return -1;
  }
  return cuda_check(cuCtxSynchronize(), "cuCtxSynchronize");
}

static void destroy_texture(SharedCudaTexture *texture) {
  if (!texture)
    return;
  if (texture->surface)
    cuSurfObjectDestroy(texture->surface);
  if (texture->cuda_resource)
    cuGraphicsUnregisterResource(texture->cuda_resource);
  if (texture->egl_image)
    texlink_egl_image_destroy(texture->egl_image);
  if (texture->texture_frame)
    texlink_egl_texture_frame_destroy(texture->texture_frame);
  memset(texture, 0, sizeof(*texture));
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "texlink";

  if (glfwInit() != GLFW_TRUE) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  GLFWwindow *win =
      glfwCreateWindow(WIDTH, HEIGHT, "cuda tex producer", NULL, NULL);
  if (!win) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(win);
  glewInit();

  CUcontext cuda_context = NULL;
  CUmodule module = NULL;
  CUfunction pitch_kernel = NULL;
  CUfunction surface_kernel = NULL;
  if (init_cuda(&cuda_context) != 0 ||
      build_kernels(&module, &pitch_kernel, &surface_kernel) != 0) {
    glfwTerminate();
    return 1;
  }

  EGLDisplay display = eglGetCurrentDisplay();
  SharedCudaTexture textures[FRAME_COUNT];
  memset(textures, 0, sizeof(textures));
  texlink_frame_t *frames[FRAME_COUNT] = {0};
  for (int i = 0; i < FRAME_COUNT; i++) {
    texlink_egl_texture_frame_t *texture_frame =
        texlink_egl_texture_frame_create(&(texlink_egl_texture_frame_desc_t){
            .display = display,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
        });
    if (!texture_frame) {
      fprintf(stderr, "texlink_egl_texture_frame_create failed for slot %d\n",
              i);
      for (int j = 0; j < FRAME_COUNT; j++)
        destroy_texture(&textures[j]);
      glfwTerminate();
      return 1;
    }
    if (import_texture(display, texture_frame, &textures[i]) != 0) {
      fprintf(stderr, "CUDA texture import failed for slot %d\n", i);
      for (int j = 0; j < FRAME_COUNT; j++)
        destroy_texture(&textures[j]);
      glfwTerminate();
      return 1;
    }
    frames[i] = texlink_egl_texture_frame_frame(texture_frame);
    printf("slot=%d cuda_frame=%s %ux%u pitch=%u\n", i,
           textures[i].cuda_frame.frameType == CU_EGL_FRAME_TYPE_ARRAY
               ? "array"
               : "pitch",
           textures[i].cuda_frame.width, textures[i].cuda_frame.height,
           textures[i].cuda_frame.pitch);
  }

  printf("Serving \"%s\" with CUDA-written textures\n", name);
  texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
      .name = name,
      .backend_type = TEXLINK_BACKEND_EGL,
      .frames = frames,
      .frame_count = FRAME_COUNT,
  });
  if (!server || texlink_server_start(server) < 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    for (int i = 0; i < FRAME_COUNT; i++)
      destroy_texture(&textures[i]);
    glfwTerminate();
    return 1;
  }

  double last_frame = glfwGetTime();
  uint64_t frame_id = 0;
  while (!glfwWindowShouldClose(win)) {
    texlink_server_poll(server);
    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;

    int idx = texlink_frame_index(frame);
    if (idx >= 0 && idx < FRAME_COUNT &&
        fill_texture(pitch_kernel, surface_kernel, &textures[idx],
                     (unsigned int)frame_id) == 0) {
      texlink_server_end_frame(server, frame);
      printf("frame=%" PRIu64 " slot=%d\n", frame_id + 1, idx);
    }

    glfwPollEvents();
    sleep_until_next_frame(&last_frame, 1.0 / 60.0);
    frame_id++;
  }

  texlink_server_destroy(server);
  for (int i = 0; i < FRAME_COUNT; i++)
    destroy_texture(&textures[i]);
  if (module)
    cuModuleUnload(module);
  cuCtxDestroy(cuda_context);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
