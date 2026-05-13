#define _POSIX_C_SOURCE 199309L
#define GLFW_INCLUDE_NONE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <texlink_egl.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WIDTH 512
#define HEIGHT 512

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

static const char *vert_src =
    "#version 330 core\n"
    "in vec3 position;\n"
    "in vec4 color;\n"
    "out vec4 v_color;\n"
    "uniform float angle;\n"
    "void main() {\n"
    "  float c = cos(angle), s = sin(angle);\n"
    "  mat2 rot = mat2(c, -s, s, c);\n"
    "  gl_Position = vec4(rot * position.xy, 0.0, 1.0);\n"
    "  v_color = color;\n"
    "}\n";

static const char *frag_src = "#version 330 core\n"
                              "in vec4 v_color;\n"
                              "out vec4 FragColor;\n"
                              "void main() { FragColor = v_color; }\n";

static float verts[] = {
    0.0f,  0.577350f,  0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    -0.5f, -0.288675f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
    0.5f,  -0.288675f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
};

int main(void) {
  glfwInit();
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  GLFWwindow *win =
      glfwCreateWindow(WIDTH, HEIGHT, "egl tex producer", NULL, NULL);
  glfwMakeContextCurrent(win);
  glewInit();

  EGLDisplay dpy = eglGetCurrentDisplay();
  texlink_egl_texture_frame_t *texture_frames[2] = {
      texlink_egl_texture_frame_create(&(texlink_egl_texture_frame_desc_t){
          .version = 1,
          .display = dpy,
          .width = WIDTH,
          .height = HEIGHT,
          .format = TEXLINK_FRAME_FORMAT_ARGB8888,
      }),
      texlink_egl_texture_frame_create(&(texlink_egl_texture_frame_desc_t){
          .version = 1,
          .display = dpy,
          .width = WIDTH,
          .height = HEIGHT,
          .format = TEXLINK_FRAME_FORMAT_ARGB8888,
      }),
  };
  texlink_frame_t *frames[2] = {
      texlink_egl_texture_frame_frame(texture_frames[0]),
      texlink_egl_texture_frame_frame(texture_frames[1]),
  };
  if (!frames[0] || !frames[1]) {
    fprintf(stderr, "texlink_egl_texture_frame_create failed\n");
    return 1;
  }

  printf("Serving 'texshare'...\n");
  texlink_server_desc_t desc = {
      .version = 1,
      .name = "texshare",
      .backend = TEXLINK_BACKEND_EGL,
      .frames = frames,
      .frame_count = 2,
  };
  texlink_server_t *server = texlink_server_create(&desc);
  if (!server || texlink_server_start(server) < 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    return 1;
  }
  printf("Rendering...\n");

  GLuint textures[2], fbos[2], rbos[2];
  for (int i = 0; i < 2; i++) {
    textures[i] = texlink_egl_texture_frame_texture(texture_frames[i]);
    glBindTexture(GL_TEXTURE_2D, textures[i]);

    glGenFramebuffers(1, &fbos[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           textures[i], 0);

    glGenRenderbuffers(1, &rbos[i]);
    glBindRenderbuffer(GL_RENDERBUFFER, rbos[i]);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, WIDTH, HEIGHT);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, rbos[i]);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "FBO %d incomplete\n", i);
      return 1;
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  GLuint vert = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vert, 1, &vert_src, NULL);
  glCompileShader(vert);
  GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(frag, 1, &frag_src, NULL);
  glCompileShader(frag);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glBindAttribLocation(prog, 0, "position");
  glBindAttribLocation(prog, 1, "color");
  glLinkProgram(prog);
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint angle_loc = glGetUniformLocation(prog, "angle");

  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
                        (void *)(3 * sizeof(float)));
  glBindVertexArray(0);

  double last_frame = glfwGetTime();
  while (!glfwWindowShouldClose(win)) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;
    int idx = texlink_frame_index(frame);

    float angle = (float)glfwGetTime();

    glBindFramebuffer(GL_FRAMEBUFFER, fbos[idx]);
    glViewport(0, 0, WIDTH, HEIGHT);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(prog);
    glUniform1f(angle_loc, angle);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);

    glFlush();
    texlink_server_end_frame(server, frame);

    /* Blit shared FBO to window for preview */
    int win_w, win_h;
    glfwGetFramebufferSize(win, &win_w, &win_h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[idx]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, WIDTH, HEIGHT, 0, 0, win_w, win_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glfwSwapBuffers(win);

    sleep_until_next_frame(&last_frame, 1.0 / 60.0);
    glfwPollEvents();
  }

  texlink_server_destroy(server);
  texlink_egl_texture_frame_destroy(texture_frames[0]);
  texlink_egl_texture_frame_destroy(texture_frames[1]);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
