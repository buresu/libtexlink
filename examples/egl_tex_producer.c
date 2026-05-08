#define _GNU_SOURCE
#define GLFW_INCLUDE_NONE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <dmabuflink.h>
#include <drm_fourcc.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define WIDTH 640
#define HEIGHT 480

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
    0.0f, 0.5f, 0.0f, 1.0f, 0.0f,  0.0f, 1.0f, -0.5f, -0.5f, 0.0f, 0.0f,
    1.0f, 0.0f, 1.0f, 0.5f, -0.5f, 0.0f, 0.0f, 0.0f,  1.0f,  1.0f,
};

typedef PFNGLEGLIMAGETARGETTEXTURE2DOESPROC TargetTex2D_fn;

static EGLImage import_dma_buf(EGLDisplay dpy, int fd, const dmabl_meta_t *m) {
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
      glfwCreateWindow(WIDTH, HEIGHT, "egl_tex_producer", NULL, NULL);
  glfwMakeContextCurrent(win);
  glewInit();

  TargetTex2D_fn glEGLImageTargetTexture2DOES =
      (TargetTex2D_fn)eglGetProcAddress("glEGLImageTargetTexture2DOES");
  if (!glEGLImageTargetTexture2DOES) {
    fprintf(stderr, "GL_OES_EGL_image not supported\n");
    return 1;
  }

  dmabl_buf_t *bufs[2] = {
      dmabl_alloc(WIDTH, HEIGHT, DRM_FORMAT_ARGB8888, DMABL_TYPE_TEXTURE_2D),
      dmabl_alloc(WIDTH, HEIGHT, DRM_FORMAT_ARGB8888, DMABL_TYPE_TEXTURE_2D),
  };
  if (!bufs[0] || !bufs[1]) {
    fprintf(stderr, "dmabl_alloc failed\n");
    return 1;
  }

  printf("Serving 'texshare', waiting for consumer...\n");
  dmabl_session_t *session =
      dmabl_serve_named("texshare", bufs, DMABL_BUFFERING_DOUBLE);
  if (!session) {
    fprintf(stderr, "dmabl_serve_named failed\n");
    return 1;
  }
  printf("Consumer connected. Rendering...\n");

  EGLDisplay dpy = eglGetCurrentDisplay();
  dmabl_meta_t meta = dmabl_session_meta(session);

  EGLImage images[2];
  GLuint textures[2], fbos[2], rbos[2];
  for (int i = 0; i < 2; i++) {
    images[i] = import_dma_buf(dpy, dmabl_get_dma_fd(bufs[i]), &meta);
    if (images[i] == EGL_NO_IMAGE) {
      fprintf(stderr, "eglCreateImage failed for buf %d\n", i);
      return 1;
    }

    glGenTextures(1, &textures[i]);
    glBindTexture(GL_TEXTURE_2D, textures[i]);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, images[i]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

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
    int idx = dmabl_producer_begin(session);
    if (idx < 0)
      break;

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
    dmabl_producer_end(session, idx);

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

  dmabl_session_close(session);
  dmabl_free(bufs[0]);
  dmabl_free(bufs[1]);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
