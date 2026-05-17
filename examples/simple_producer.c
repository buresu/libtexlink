#define _POSIX_C_SOURCE 199309L
/*
 * simple producer example
 *
 * Usage: producer [session_name]
 *        Default name: "example"
 *        Socket:       /tmp/texlink/<name>.sock
 */
#include <texlink.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#define BUF_SIZE 65536 /* 64 KiB */

static void sleep_ms(int ms) {
#ifdef _WIN32
  Sleep((DWORD)ms);
#else
  struct timespec ts = {
      .tv_sec = ms / 1000,
      .tv_nsec = (ms % 1000) * 1000000L,
  };
  nanosleep(&ts, NULL);
#endif
}

int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);

  const char *name = (argc > 1) ? argv[1] : "example";

  /* Double-buffering: two raw frames */
  texlink_frame_t *frames[2] = {
      texlink_frame_create(&(texlink_frame_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_RAW,
          .size = BUF_SIZE,
      }),
      texlink_frame_create(&(texlink_frame_desc_t){
          .frame_type = TEXLINK_FRAME_TYPE_RAW,
          .size = BUF_SIZE,
      }),
  };

  if (!frames[0] || !frames[1]) {
    fprintf(stderr, "texlink_frame_create failed\n");
    return 1;
  }

  texlink_mapping_t mappings[2] = {0};
  texlink_map_desc_t map_desc = {
      .flags = TEXLINK_MAP_WRITE,
      .offset = 0,
      .size = 0,
  };
  for (int i = 0; i < 2; i++) {
    if (texlink_frame_map(frames[i], &map_desc, &mappings[i]) != 0) {
      fprintf(stderr, "texlink_frame_map failed\n");
      return 1;
    }
  }

  /*
   * texlink_server_start():
   *   1. bind socket at /tmp/texlink/<name>.sock
   *   2. register name in global registry
   *   3. poll accepts consumers while frames are produced
   */
  printf("Serving \"%s\"...\n", name);
  texlink_server_desc_t desc = {
      .name = name,
      .backend_type = TEXLINK_BACKEND_CPU,
      .frames = frames,
      .frame_count = 2,
  };
  texlink_server_t *server = texlink_server_create(&desc);
  if (!server || texlink_server_start(server) < 0) {
    fprintf(stderr, "texlink_server_start failed\n");
    return 1;
  }
  printf("Producing frames...\n");

  uint64_t counter = 0;
  while (1) {
    texlink_server_poll(server);

    texlink_frame_t *frame = texlink_server_begin_frame(server);
    if (!frame)
      break;

    texlink_cpu_access_desc_t access_desc = {
        .flags = TEXLINK_MAP_WRITE,
        .offset = 0,
        .size = 0,
    };
    if (texlink_frame_cpu_begin(frame, &access_desc) == 0) {
      uint64_t *data = mappings[texlink_frame_index(frame)].data;
      counter++;
      data[0] = counter;
      memset(data + 1, (int)(counter & 0xff), BUF_SIZE - sizeof(uint64_t));
      texlink_frame_cpu_end(frame, &access_desc);
    }

    texlink_server_end_frame(server, frame);
    printf("frame=%" PRIu64 "  slot=%d\n", counter, texlink_frame_index(frame));
    sleep_ms(16);
  }

  texlink_server_destroy(server); /* also unregisters from registry */
  texlink_frame_unmap(frames[0]);
  texlink_frame_unmap(frames[1]);
  texlink_frame_destroy(frames[0]);
  texlink_frame_destroy(frames[1]);
  return 0;
}
