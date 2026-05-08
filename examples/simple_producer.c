#define _GNU_SOURCE
/*
 * simple producer example
 *
 * Usage: producer [session_name]
 *        Default name: "example"
 *        Socket:       /tmp/dmabl/<name>.sock
 */
#include <dmabuflink.h>

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
  dmabl_buf_t *bufs[2] = {
      dmabl_alloc(BUF_SIZE, 1, 0, DMABL_TYPE_RAW, DMABL_BACKEND_CPU),
      dmabl_alloc(BUF_SIZE, 1, 0, DMABL_TYPE_RAW, DMABL_BACKEND_CPU),
  };

  if (!bufs[0] || !bufs[1]) {
    fprintf(stderr, "dmabl_alloc failed\n");
    return 1;
  }

  /*
   * dmabl_serve_named():
   *   1. bind socket at /tmp/dmabl/<name>.sock
   *   2. register name in global registry  ← consumer can find it now
   *   3. block waiting for consumer connection
   */
  printf("Registering \"%s\", waiting for consumer...\n", name);
  dmabl_session_t *s = dmabl_serve_named(name, bufs, DMABL_BUFFERING_DOUBLE);
  if (!s) {
    fprintf(stderr, "dmabl_serve_named failed\n");
    return 1;
  }
  printf("Consumer connected. Producing frames...\n");

  uint64_t counter = 0;
  while (1) {
    int idx = dmabl_producer_begin(s);
    if (idx < 0)
      break;

    uint64_t *data = dmabl_map_cpu(bufs[idx]);
    if (data) {
      counter++;
      dmabl_cpu_begin(bufs[idx], 1);
      data[0] = counter;
      memset(data + 1, (int)(counter & 0xff), BUF_SIZE - sizeof(uint64_t));
      dmabl_cpu_end(bufs[idx], 1);
    }

    dmabl_producer_end(s, idx);
    printf("frame=%" PRIu64 "  buf=%d\n", counter, idx);
    sleep_ms(16);
  }

  dmabl_session_close(s); /* also unregisters from registry */
  dmabl_free(bufs[0]);
  dmabl_free(bufs[1]);
  return 0;
}
