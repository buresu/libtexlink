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
      texlink_alloc(BUF_SIZE, 1, 0, TEXLINK_TYPE_RAW, TEXLINK_BACKEND_CPU),
      texlink_alloc(BUF_SIZE, 1, 0, TEXLINK_TYPE_RAW, TEXLINK_BACKEND_CPU),
  };

  if (!bufs[0] || !bufs[1]) {
    fprintf(stderr, "texlink_alloc failed\n");
    return 1;
  }

  /*
   * texlink_serve_named():
   *   1. bind socket at /tmp/texlink/<name>.sock
   *   2. register name in global registry  ← consumer can find it now
   *   3. block waiting for consumer connection
   */
  printf("Registering \"%s\", waiting for consumer...\n", name);
  texlink_session_t *s = texlink_serve_named(name, bufs, TEXLINK_BUFFERING_DOUBLE);
  if (!s) {
    fprintf(stderr, "texlink_serve_named failed\n");
    return 1;
  }
  printf("Consumer connected. Producing frames...\n");

  uint64_t counter = 0;
  while (1) {
    int idx = texlink_producer_begin(s);
    if (idx < 0)
      break;

    uint64_t *data = texlink_map_cpu(bufs[idx]);
    if (data) {
      counter++;
      texlink_cpu_begin(bufs[idx], 1);
      data[0] = counter;
      memset(data + 1, (int)(counter & 0xff), BUF_SIZE - sizeof(uint64_t));
      texlink_cpu_end(bufs[idx], 1);
    }

    texlink_producer_end(s, idx);
    printf("frame=%" PRIu64 "  buf=%d\n", counter, idx);
    sleep_ms(16);
  }

  texlink_session_close(s); /* also unregisters from registry */
  texlink_free(bufs[0]);
  texlink_free(bufs[1]);
  return 0;
}
