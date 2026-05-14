#include <texlink_wgl.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <windows.h>

#define WIDTH 512
#define HEIGHT 512
#define FRAME_COUNT 2

static void sleep_until_next_frame(double *last_time, double interval_sec) {
  double now = glfwGetTime();
  double wait = *last_time + interval_sec - now;
  if (wait > 0.0)
    Sleep((DWORD)(wait * 1000.0));
  *last_time = glfwGetTime();
}

static void draw_rotating_triangle(float angle) {
  float c = cosf(angle);
  float s = sinf(angle);
  const float verts[3][2] = {
      {0.0f, 0.577350f},
      {-0.5f, -0.288675f},
      {0.5f, -0.288675f},
  };
  const float colors[3][3] = {
      {1.0f, 0.0f, 0.0f},
      {0.0f, 1.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
  };

  glBegin(GL_TRIANGLES);
  for (int i = 0; i < 3; i++) {
    float x = verts[i][0];
    float y = verts[i][1];
    glColor3f(colors[i][0], colors[i][1], colors[i][2]);
    glVertex2f(c * x - s * y, s * x + c * y);
  }
  glEnd();
}

static void render_scene(int width, int height, float angle) {
  glViewport(0, 0, width, height);
  glDisable(GL_DEPTH_TEST);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  draw_rotating_triangle(angle);
}

static int copy_window_to_shared_texture(texlink_wgl_texture_frame_t *frame) {
  if (texlink_wgl_texture_frame_lock(frame) != 0) {
    fprintf(stderr, "texlink_wgl_texture_frame_lock failed: %s\n",
            texlink_wgl_last_error_string());
    return -1;
  }

  GLuint texture = texlink_wgl_texture_frame_texture(frame);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, WIDTH, HEIGHT);
  glBindTexture(GL_TEXTURE_2D, 0);
  glFlush();

  GLenum err = glGetError();
  texlink_wgl_texture_frame_unlock(frame);
  if (err != GL_NO_ERROR) {
    fprintf(stderr, "glCopyTexSubImage2D failed: 0x%04x\n", err);
    return -1;
  }
  return 0;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "wgl_example";

  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
  GLFWwindow *window =
      glfwCreateWindow(WIDTH, HEIGHT, "texlink wgl producer", NULL, NULL);
  if (!window) {
    fprintf(stderr, "glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  texlink_wgl_texture_frame_t *texture_frames[FRAME_COUNT] = {0};
  texlink_frame_t *frames[FRAME_COUNT] = {0};
  for (int i = 0; i < FRAME_COUNT; i++) {
    texture_frames[i] =
        texlink_wgl_texture_frame_create(&(texlink_wgl_texture_frame_desc_t){
            .version = 1,
            .width = WIDTH,
            .height = HEIGHT,
            .format = TEXLINK_FRAME_FORMAT_ARGB8888,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_wgl_texture_frame_create failed: %s\n",
              texlink_wgl_last_error_string());
      glfwTerminate();
      return 1;
    }
    frames[i] = texlink_wgl_texture_frame_frame(texture_frames[i]);
  }

  texlink_server_t *server = texlink_server_create(&(texlink_server_desc_t){
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_WGL,
      .frames = frames,
      .frame_count = FRAME_COUNT,
  });
  if (!server || texlink_server_start(server) != 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    for (int i = 0; i < FRAME_COUNT; i++)
      texlink_wgl_texture_frame_destroy(texture_frames[i]);
    glfwTerminate();
    return 1;
  }

  printf("Serving \"%s\" with %d WGL textures\n", name, FRAME_COUNT);
  double last_frame = glfwGetTime();
  while (!glfwWindowShouldClose(window)) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;
    int idx = texlink_frame_index(frame);
    if (idx < 0 || idx >= FRAME_COUNT)
      idx = 0;

    float angle = (float)glfwGetTime();
    render_scene(WIDTH, HEIGHT, angle);
    if (copy_window_to_shared_texture(texture_frames[idx]) != 0)
      break;
    texlink_server_end_frame(server, frame);

    glfwSwapBuffers(window);
    sleep_until_next_frame(&last_frame, 1.0 / 60.0);
    glfwPollEvents();
  }

  texlink_server_destroy(server);
  for (int i = 0; i < FRAME_COUNT; i++)
    texlink_wgl_texture_frame_destroy(texture_frames[i]);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
