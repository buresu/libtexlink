/*
 * CUDA texture consumer example
 *
 * Usage: cuda_tex_consumer [session_name]
 *        cuda_tex_consumer --list
 *        Default name: "texlink"
 *
 * This imports the producer's DMA-BUF texture as an EGLImage, registers that
 * EGLImage with CUDA, and prints the CUDA-visible frame description.
 */
#include <EGL/egl.h>
#include <cuda.h>
#include <cudaEGL.h>

#include <texlink_egl.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 3

typedef struct {
  texlink_egl_image_t *egl_image;
  CUgraphicsResource cuda_resource;
  CUeglFrame egl_frame;
} ImportedCudaTexture;

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

static const char *egl_frame_type_name(CUeglFrameType type) {
  switch (type) {
  case CU_EGL_FRAME_TYPE_ARRAY:
    return "array";
  case CU_EGL_FRAME_TYPE_PITCH:
    return "pitch";
  default:
    return "unknown";
  }
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

static int init_egl(EGLDisplay *out_display) {
  PFNEGLQUERYDEVICESEXTPROC query_devices =
      (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
          "eglGetPlatformDisplayEXT");

  if (query_devices && get_platform_display) {
    EGLDeviceEXT devices[16];
    EGLint device_count = 0;
    if (query_devices(16, devices, &device_count)) {
      for (EGLint i = 0; i < device_count; i++) {
        EGLDisplay display = get_platform_display(EGL_PLATFORM_DEVICE_EXT,
                                                  devices[i], NULL);
        if (display == EGL_NO_DISPLAY)
          continue;

        EGLint major = 0;
        EGLint minor = 0;
        if (!eglInitialize(display, &major, &minor))
          continue;

        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (extensions &&
            strstr(extensions, "EGL_EXT_image_dma_buf_import")) {
          printf("EGL %d.%d initialized via EGLDevice %d.\n", major, minor,
                 i);
          *out_display = display;
          return 0;
        }

        eglTerminate(display);
      }
    }
  }

  EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (display == EGL_NO_DISPLAY) {
    fprintf(stderr, "eglGetDisplay failed\n");
    return -1;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(display, &major, &minor)) {
    fprintf(stderr, "eglInitialize failed\n");
    return -1;
  }

  printf("EGL %d.%d initialized.\n", major, minor);
  *out_display = display;
  return 0;
}

static void destroy_imported(ImportedCudaTexture *texture) {
  if (!texture)
    return;
  if (texture->cuda_resource)
    cuGraphicsUnregisterResource(texture->cuda_resource);
  if (texture->egl_image)
    texlink_egl_image_destroy(texture->egl_image);
  memset(texture, 0, sizeof(*texture));
}

static int import_frame(EGLDisplay display, texlink_frame_t *frame,
                        ImportedCudaTexture *out_texture) {
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
      CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY);
  if (cuda_check(result, "cuGraphicsEGLRegisterImage") != 0)
    return -1;

  result = cuGraphicsResourceGetMappedEglFrame(&out_texture->egl_frame,
                                               out_texture->cuda_resource, 0, 0);
  return cuda_check(result, "cuGraphicsResourceGetMappedEglFrame");
}

static int list_sessions(void) {
  char names[TEXLINK_NAME_MAX * 16][TEXLINK_NAME_MAX];
  int count = texlink_registry_list(names, 16);
  if (count == 0) {
    printf("No active sessions found.\n");
    return 0;
  }

  printf("Active sessions (%d):\n", count);
  for (int i = 0; i < count; i++)
    printf("  %s\n", names[i]);
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  if (argc > 1 && strcmp(argv[1], "--list") == 0)
    return list_sessions();

  const char *name = (argc > 1) ? argv[1] : "texlink";

  CUcontext cuda_context = NULL;
  EGLDisplay display = EGL_NO_DISPLAY;
  if (init_cuda(&cuda_context) != 0 || init_egl(&display) != 0)
    return 1;

  printf("Connecting to \"%s\" ...\n", name);
  texlink_client_desc_t desc = {
      .name = name,
      .backend_type = TEXLINK_BACKEND_CUDA,
      .timeout_ms = 5000,
  };
  texlink_client_t *client = texlink_client_create(&desc);
  if (!client || texlink_client_connect(client) < 0) {
    fprintf(stderr, "Session \"%s\" not found or connection failed.\n", name);
    fprintf(stderr, "Start a texture producer first, for example:\n");
    fprintf(stderr, "  ./vulkan_tex_producer        # session: texlink\n");
    fprintf(stderr, "  ./egl_tex_producer           # session: texlink\n");
    fprintf(stderr, "Then run:\n");
    fprintf(stderr, "  ./cuda_tex_consumer texlink\n");
    fprintf(stderr, "Use ./cuda_tex_consumer --list to show active sessions.\n");
    if (client)
      texlink_client_destroy(client);
    eglTerminate(display);
    cuCtxDestroy(cuda_context);
    return 1;
  }
  printf("Connected.\n");

  texlink_meta_t meta = texlink_client_meta(client);
  printf("Texture: %ux%u format=0x%x stride=%u size=%" PRIu64 "\n",
         meta.width, meta.height, meta.format, meta.stride, meta.size);

  ImportedCudaTexture textures[MAX_FRAMES];
  memset(textures, 0, sizeof(textures));

  int frame_count = texlink_client_frame_count(client);
  if (frame_count > MAX_FRAMES)
    frame_count = MAX_FRAMES;

  for (int i = 0; i < frame_count; i++) {
    texlink_frame_t *frame = texlink_client_frame(client, i);
    if (!frame)
      break;
    if (import_frame(display, frame, &textures[i]) != 0) {
      texlink_client_destroy(client);
      for (int j = 0; j < frame_count; j++)
        destroy_imported(&textures[j]);
      eglTerminate(display);
      cuCtxDestroy(cuda_context);
      return 1;
    }

    CUeglFrame *f = &textures[i].egl_frame;
    printf("slot=%d cuda_frame=%s %ux%u pitch=%u planes=%u channels=%u\n", i,
           egl_frame_type_name(f->frameType), f->width, f->height, f->pitch,
           f->planeCount, f->numChannels);
  }

  while (1) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    texlink_meta_t cur_meta = texlink_client_meta(client);
    int idx = texlink_frame_index(frame);
    if (idx >= 0 && idx < frame_count) {
      CUeglFrame *f = &textures[idx].egl_frame;
      printf("frame=%" PRIu64 " slot=%d cuda_frame=%s %ux%u\n",
             cur_meta.frame_id, idx, egl_frame_type_name(f->frameType),
             f->width, f->height);
    }

    cuCtxSynchronize();
    texlink_client_release_frame(client, frame);
  }

  texlink_client_destroy(client);
  for (int i = 0; i < frame_count; i++)
    destroy_imported(&textures[i]);
  eglTerminate(display);
  cuCtxDestroy(cuda_context);
  return 0;
}
