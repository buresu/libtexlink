#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

#include <texlink_cgl.h>

#include <stdio.h>

#define WIDTH 512
#define HEIGHT 512
#define MAX_SAMPLE_FRAMES 3

static const char *vert_src = "#version 150 core\n"
                              "in vec2 position;\n"
                              "in vec2 texcoord;\n"
                              "out vec2 uv;\n"
                              "uniform vec2 texture_size;\n"
                              "uniform int flip_y;\n"
                              "void main() {\n"
                              "  uv = texcoord;\n"
                              "  if (flip_y != 0) uv.y = texture_size.y - uv.y;\n"
                              "  gl_Position = vec4(position, 0.0, 1.0);\n"
                              "}\n";

static const char *frag_src =
    "#version 150 core\n"
    "uniform sampler2DRect tex;\n"
    "in vec2 uv;\n"
    "out vec4 FragColor;\n"
    "void main() { FragColor = texture(tex, uv); }\n";

static int check_shader(GLuint shader, const char *name) {
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok)
    return 0;
  char log[1024];
  glGetShaderInfoLog(shader, sizeof(log), NULL, log);
  fprintf(stderr, "%s shader compile failed: %s\n", name, log);
  return -1;
}

static int check_program(GLuint program) {
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok)
    return 0;
  char log[1024];
  glGetProgramInfoLog(program, sizeof(log), NULL, log);
  fprintf(stderr, "program link failed: %s\n", log);
  return -1;
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const char *name = (argc > 1) ? argv[1] : "texlink";

  if (!glfwInit())
    return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
  glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
  GLFWwindow *win =
      glfwCreateWindow(WIDTH, HEIGHT, "cgl tex consumer", NULL, NULL);
  if (!win)
    return 1;
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);

  texlink_client_t *client = texlink_client_create(&(texlink_client_desc_t){
      .name = name,
      .backend_type = TEXLINK_BACKEND_CGL,
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

  texlink_cgl_texture_frame_t *texture_frames[MAX_SAMPLE_FRAMES] = {0};
  GLuint fbos[MAX_SAMPLE_FRAMES] = {0};
  for (int i = 0; i < frame_count; i++) {
    texture_frames[i] = texlink_cgl_texture_frame_import(
        &(texlink_cgl_import_desc_t){
            .frame = texlink_client_frame(client, i),
            .target = GL_TEXTURE_RECTANGLE,
        });
    if (!texture_frames[i]) {
      fprintf(stderr, "texlink_cgl_texture_frame_import failed: %s\n",
              texlink_cgl_last_error_string());
      return 1;
    }
    glGenFramebuffers(1, &fbos[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE,
        texlink_cgl_texture_frame_texture(texture_frames[i]), 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      fprintf(stderr, "CGL consumer FBO %d incomplete\n", i);
      return 1;
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  printf("Connected CGL consumer to '%s' (%ux%u).\n", name, meta.width,
         meta.height);

  GLuint vert = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vert, 1, &vert_src, NULL);
  glCompileShader(vert);
  if (check_shader(vert, "vertex") != 0)
    return 1;
  GLuint frag = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(frag, 1, &frag_src, NULL);
  glCompileShader(frag);
  if (check_shader(frag, "fragment") != 0)
    return 1;
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vert);
  glAttachShader(prog, frag);
  glBindAttribLocation(prog, 0, "position");
  glBindAttribLocation(prog, 1, "texcoord");
  glLinkProgram(prog);
  if (check_program(prog) != 0)
    return 1;
  glDeleteShader(vert);
  glDeleteShader(frag);

  GLint tex_loc = glGetUniformLocation(prog, "tex");
  GLint texture_size_loc = glGetUniformLocation(prog, "texture_size");
  GLint flip_y_loc = glGetUniformLocation(prog, "flip_y");

  float w = (float)meta.width;
  float h = (float)meta.height;
  float quad[] = {
      -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, -1.0f, w,    0.0f,
      1.0f,  1.0f,  w,    h,    -1.0f, -1.0f, 0.0f, 0.0f,
      1.0f,  1.0f,  w,    h,    -1.0f, 1.0f,  0.0f, h,
  };

  GLuint vao = 0, vbo = 0;
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
    glBindTexture(GL_TEXTURE_RECTANGLE,
                  texlink_cgl_texture_frame_texture(texture_frames[idx]));
    glUniform1i(tex_loc, 0);
    glUniform2f(texture_size_loc, (float)meta.width, (float)meta.height);
    glUniform1i(flip_y_loc,
                texlink_frame_should_flip_y((texlink_backend_t)meta.backend_type,
                                            TEXLINK_BACKEND_CGL));
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    unsigned char bgra[4] = {0};
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbos[idx]);
    glReadPixels((GLint)(meta.width / 2), (GLint)(meta.height / 2), 1, 1,
                 GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, bgra);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    texlink_meta_t cur = texlink_client_meta(client);
    printf("frame=%llu slot=%d bgra=(%u %u %u %u)\n",
           (unsigned long long)cur.frame_id, idx, bgra[0], bgra[1], bgra[2],
           bgra[3]);

    glfwSwapBuffers(win);
    texlink_client_release_frame(client, frame);
    glfwPollEvents();
  }

  for (int i = 0; i < frame_count; i++) {
    if (fbos[i])
      glDeleteFramebuffers(1, &fbos[i]);
    texlink_cgl_texture_frame_destroy(texture_frames[i]);
  }
  texlink_client_destroy(client);
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
