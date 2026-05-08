/*
 * consumer example
 *
 * Usage: consumer [session_name]
 *        With no argument: list available sessions and exit.
 *        With a name:      connect by name and consume frames.
 */
#include <dmabuflink.h>

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv) {
  if (argc < 2) {
    /* Discovery mode: list what's available */
    char names[DMABL_NAME_MAX * 16][DMABL_NAME_MAX];
    int n = dmabl_list_sessions(names, 16);
    if (n == 0) {
      printf("No active sessions found.\n");
    } else {
      printf("Active sessions (%d):\n", n);
      for (int i = 0; i < n; i++)
        printf("  [%d] %s\n", i, names[i]);
    }
    return 0;
  }

  const char *name = argv[1];
  printf("Connecting to \"%s\" ...\n", name);

  dmabl_session_t *s = dmabl_connect_by_name(name);
  if (!s) {
    fprintf(stderr, "Session \"%s\" not found or connection failed.\n", name);
    return 1;
  }
  printf("Connected.\n");

  dmabl_meta_t meta = dmabl_session_meta(s);
  printf("Buffer: %u bytes  format=0x%x\n", meta.size, meta.format);

  while (1) {
    int idx = dmabl_consumer_acquire(s);
    if (idx < 0) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    dmabl_buf_t *buf = dmabl_session_buf(s, idx);
    dmabl_meta_t cur_meta = dmabl_session_meta(s);
    uint64_t *data = dmabl_map_cpu(buf);
    if (data) {
      dmabl_cpu_begin(buf, 0);
      uint64_t val = data[0];
      dmabl_cpu_end(buf, 0);
      printf("frame=%" PRIu64 "  buf=%d  val=%" PRIu64 "\n", cur_meta.frame_id,
             idx, val);
    }

    dmabl_consumer_release(s, idx);
  }

  dmabl_session_close(s);
  return 0;
}
