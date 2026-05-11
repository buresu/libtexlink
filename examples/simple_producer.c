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

  /* Double-buffering: two raw buffers */
  texlink_buf_t *bufs[2] = {
      texlink_buf_alloc(BUF_SIZE, 1, 0, TEXLINK_TYPE_RAW),
      texlink_buf_alloc(BUF_SIZE, 1, 0, TEXLINK_TYPE_RAW),
  };

  if (!bufs[0] || !bufs[1]) {
    fprintf(stderr, "texlink_buf_alloc failed\n");
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
      .bufs = bufs,
      .buffer_count = 2,
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

    int idx = texlink_server_begin_frame(server);
    if (idx < 0)
      break;

    uint64_t *data = texlink_buf_map(bufs[idx]);
    if (data) {
      counter++;
      texlink_buf_begin_access(bufs[idx], TEXLINK_ACCESS_WRITE);
      data[0] = counter;
      memset(data + 1, (int)(counter & 0xff), BUF_SIZE - sizeof(uint64_t));
      texlink_buf_end_access(bufs[idx], TEXLINK_ACCESS_WRITE);
    }

    texlink_server_end_frame(server, idx);
    printf("frame=%" PRIu64 "  buf=%d\n", counter, idx);
    sleep_ms(16);
  }

  texlink_server_destroy(server); /* also unregisters from registry */
  texlink_buf_free(bufs[0]);
  texlink_buf_free(bufs[1]);
  return 0;
}
