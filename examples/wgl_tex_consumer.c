#include <texlink_wgl.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <d3d12.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

#define WIDTH 512
#define HEIGHT 512
#define MAX_FRAMES 2

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

static void draw_textured_quad(GLuint texture, int width, int height,
                               int flip_y) {
  GLfloat v0 = flip_y ? 1.0f : 0.0f;
  GLfloat v1 = flip_y ? 0.0f : 1.0f;

  glViewport(0, 0, width, height);
  glDisable(GL_DEPTH_TEST);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glColor3f(1.0f, 1.0f, 1.0f);

  glBegin(GL_QUADS);
  glTexCoord2f(0.0f, v0);
  glVertex2f(-1.0f, -1.0f);
  glTexCoord2f(1.0f, v0);
  glVertex2f(1.0f, -1.0f);
  glTexCoord2f(1.0f, v1);
  glVertex2f(1.0f, 1.0f);
  glTexCoord2f(0.0f, v1);
  glVertex2f(-1.0f, 1.0f);
  glEnd();

  glBindTexture(GL_TEXTURE_2D, 0);
  glDisable(GL_TEXTURE_2D);
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "texlink";

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "texlink wgl consumer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
      .name = name,
      .backend_type = TEXLINK_BACKEND_WGL,
      .timeout_ms = 5000,
  });
  if (!client || texlink_client_connect(client) != 0) {
    fprintf(stderr, "texlink_client_connect failed\n");
    texlink_client_destroy(client);
    glfwTerminate();
    return 1;
  }

  texlink_meta_t meta = texlink_client_meta(client);
  int frame_count = texlink_client_frame_count(client);
  if (frame_count > MAX_FRAMES)
    frame_count = MAX_FRAMES;

  ID3D12Device *sync_device = NULL;
  D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                    (void **)&sync_device);

  texlink_wgl_texture_frame_t *texture_frames[MAX_FRAMES] = {0};
  ID3D12Fence *sync_fences[MAX_FRAMES] = {0};
  for (int i = 0; i < frame_count; i++) {
    texlink_frame_t *frame = texlink_client_frame(client, i);
    texture_frames[i] =
        texlink_wgl_texture_frame_import(&(texlink_wgl_import_desc_t){
            .frame = frame,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_wgl_texture_frame_import failed: %s\n",
              texlink_wgl_last_error_string());
      texlink_client_destroy(client);
      glfwTerminate();
      return 1;
    }
    if (sync_device) {
      texlink_native_handle_t sync_handle;
      uint64_t sync_value;
      if (texlink_frame_get_sync_native_handle(
              frame, TEXLINK_NATIVE_HANDLE_D3D12_FENCE_HANDLE,
              &sync_handle, &sync_value) == 0) {
        sync_device->lpVtbl->OpenSharedHandle(
            sync_device, (HANDLE)sync_handle.value.ptr, &IID_ID3D12Fence,
            (void **)&sync_fences[i]);
      }
    }
  }

  printf("Connected to \"%s\" with %u WGL textures\n", name, frame_count);
  while (!glfwWindowShouldClose(window)) {
    texlink_frame_t *acquired = texlink_client_acquire_frame(client);
    if (!acquired) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    int idx = texlink_frame_index(acquired);
    if (idx < 0 || idx >= frame_count)
      idx = 0;
    if (wait_shared_fence(sync_fences[idx],
                          texlink_frame_sync_value(acquired)) != 0) {
      fprintf(stderr, "D3D12 fence wait failed\n");
      texlink_client_release_frame(client, acquired);
      break;
    }
    texlink_wgl_texture_frame_t *texture_frame = texture_frames[idx];
    if (texlink_wgl_texture_frame_lock(texture_frame) != 0) {
      fprintf(stderr, "texlink_wgl_texture_frame_lock failed: %s\n",
              texlink_wgl_last_error_string());
      texlink_client_release_frame(client, acquired);
      break;
    }

    int fb_w, fb_h;
    glfwGetFramebufferSize(window, &fb_w, &fb_h);
    draw_textured_quad(
        texlink_wgl_texture_frame_texture(texture_frame), fb_w, fb_h,
        texlink_frame_should_flip_y((texlink_backend_t)meta.backend_type,
                                    TEXLINK_BACKEND_WGL));
    glFinish();
    texlink_wgl_texture_frame_unlock(texture_frame);
    texlink_client_release_frame(client, acquired);

    glfwSwapBuffers(window);
    glfwPollEvents();
  }

  for (int i = 0; i < frame_count; i++)
    texlink_wgl_texture_frame_destroy(texture_frames[i]);
  for (int i = 0; i < frame_count; i++) {
    if (sync_fences[i])
      sync_fences[i]->lpVtbl->Release(sync_fences[i]);
  }
  if (sync_device)
    sync_device->lpVtbl->Release(sync_device);
  texlink_client_destroy(client);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
