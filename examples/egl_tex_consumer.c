#define GLFW_INCLUDE_NONE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <texlink.h>

#include <stdio.h>
#include <stdlib.h>

#define WIDTH 512
#define HEIGHT 512

static const char *vert_src = "#version 330 core\n"
                              "in vec2 position;\n"
                              "in vec2 texcoord;\n"
                              "out vec2 uv;\n"
                              "uniform int flip_y;\n"
                              "void main() {\n"
                              "  uv = texcoord;\n"
                              "  if (flip_y != 0) uv.y = 1.0 - uv.y;\n"
                              "  gl_Position = vec4(position, 0.0, 1.0);\n"
                              "}\n";

static const char *frag_src = "#version 330 core\n"
                              "uniform sampler2D tex;\n"
                              "in vec2 uv;\n"
                              "out vec4 FragColor;\n"
                              "void main() { FragColor = texture(tex, uv); }\n";

static float quad[] = {
    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,  1.0f, 1.0f, 1.0f,
    -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f,  1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f,
};

typedef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC TargetTex2D_fn;

static EGLImage import_dma_buf(EGLDisplay dpy, int fd,
                               const texlink_meta_t *m) {
  EGLAttrib attrs[] = {
      EGL_WIDTH,
      (EGLAttrib)m->width,
      EGL_HEIGHT,
      (EGLAttrib)m->height,
      EGL_LINUX_DRM_FOURCC_EXT,
      (EGLAttrib)m->format,
      EGL_DMA_BUF_PLANE0_FD_EXT,
      (EGLAttrib)fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      0,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      (EGLAttrib)m->stride,
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      (EGLAttrib)(m->modifier & 0xFFFFFFFFu),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      (EGLAttrib)(m->modifier >> 32),
      EGL_NONE,
  };
  return eglCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL,
                        attrs);
}

int main(void) {
  glfwInit();
  glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow *win =
      glfwCreateWindow(WIDTH, HEIGHT, "egl tex consumer", NULL, NULL);
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);
  glewInit();

  TargetTex2D_fn glEGLImageTargetTexture2DOES =
      (TargetTex2D_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if (!glEGLImageTargetTexture2DOES) {
    fprintf(stderr, "GL_OES_EGL_image not supported\n");
    return 1;
  }

  printf("Connecting to 'texshare'...\n");
  texlink_client_desc_t desc = {
      .version = 1,
      .name = "texshare",
      .backend = TEXLINK_BACKEND_EGL,
      .timeout_ms = 5000,
  };
  texlink_client_t *client = texlink_client_create(&desc);
  if (!client || texlink_client_connect(client) < 0) {
    fprintf(stderr, "texlink_client_connect failed\n");
    return 1;
  }
  printf("Connected.\n");

  EGLDisplay dpy = eglGetCurrentDisplay();
  texlink_meta_t meta = texlink_client_meta(client);

  EGLImage images[2];
  GLuint textures[2];
  uint32_t frame_count = texlink_client_frame_count(client);
  if (frame_count > 2)
    frame_count = 2;
  for (uint32_t i = 0; i < frame_count; i++) {
    texlink_frame_t *frame = texlink_client_frame(client, i);
    if (!frame)
      break;

    texlink_native_handle_t handle;
    if (texlink_frame_get_native_handle(frame, TEXLINK_NATIVE_HANDLE_DMA_BUF_FD,
                                        &handle) != 0) {
      fprintf(stderr, "texlink_frame_get_native_handle failed for frame %u\n",
              i);
      return 1;
    }

    images[i] = import_dma_buf(dpy, handle.value.fd, &meta);
    if (images[i] == EGL_NO_IMAGE) {
      fprintf(stderr, "eglCreateImage failed for frame %u\n", i);
      return 1;
    }

    glGenTextures(1, &textures[i]);
    glBindTexture(GL_TEXTURE_2D, textures[i]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

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
  glBindAttribLocation(prog, 1, "texcoord");
  glLinkProgram(prog);
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint tex_loc = glGetUniformLocation(prog, "tex");
  GLint flip_y_loc = glGetUniformLocation(prog, "flip_y");

  GLuint vao, vbo;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glGenBuffers(1, &vbo);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                        (void *)(2 * sizeof(float)));
  glBindVertexArray(0);

  while (!glfwWindowShouldClose(win)) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }
    int idx = texlink_frame_index(frame);

    int fb_w, fb_h;
    glfwGetFramebufferSize(win, &fb_w, &fb_h);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[idx]);
    glUniform1i(tex_loc, 0);
    glUniform1i(flip_y_loc, meta.backend != TEXLINK_BACKEND_EGL ? 1 : 0);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    texlink_client_release_frame(client, frame);

    glfwSwapBuffers(win);
    glfwPollEvents();
  }

  texlink_client_destroy(client);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
