#define _GNU_SOURCE
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
#include <unistd.h>

#define BUF_SIZE 65536 /* 64 KiB */

static void sleep_ms(int ms) {
  struct timespec ts = {
      .tv_sec = ms / 1000,
      .tv_nsec = (ms % 1000) * 1000000L,
  };
  nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
  const char *name = (argc > 1) ? argv[1] : "example";

  /* Double-buffering: two raw frames */
  texlink_frame_t *frames[2] = {
      texlink_frame_create(&(texlink_frame_desc_t){
          .version = 1,
          .type = TEXLINK_TYPE_RAW,
          .size = BUF_SIZE,
      }),
      texlink_frame_create(&(texlink_frame_desc_t){
          .version = 1,
          .type = TEXLINK_TYPE_RAW,
          .size = BUF_SIZE,
      }),
  };

  if (!frames[0] || !frames[1]) {
    fprintf(stderr, "texlink_frame_create failed\n");
    return 1;
  }

  /*
   * texlink_server_start():
   *   1. bind socket at /tmp/texlink/<name>.sock
   *   2. register name in global registry
   *   3. poll accepts consumers while frames are produced
   */
  printf("Serving \"%s\"...\n", name);
  texlink_server_desc_t desc = {
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_CPU,
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

    uint64_t *data = texlink_frame_begin_access(frame, TEXLINK_ACCESS_WRITE);
    if (data) {
      counter++;
      data[0] = counter;
      memset(data + 1, (int)(counter & 0xff), BUF_SIZE - sizeof(uint64_t));
      texlink_frame_end_access(frame);
    }

    texlink_server_end_frame(server, frame);
    printf("frame=%" PRIu64 "  slot=%d\n", counter, texlink_frame_index(frame));
    sleep_ms(16);
  }

  texlink_server_destroy(server); /* also unregisters from registry */
  texlink_frame_destroy(frames[0]);
  texlink_frame_destroy(frames[1]);
  return 0;
}
