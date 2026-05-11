/*
 * simple consumer example
 *
 * Usage: consumer [session_name]
 *        Default name: "example"
 *        Use --list to list available sessions.
 */
#include <texlink.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "--list") == 0) {
    char names[TEXLINK_NAME_MAX * 16][TEXLINK_NAME_MAX];
    int n = texlink_registry_list(names, 16);
    if (n == 0) {
      printf("No active sessions found.\n");
    } else {
      printf("Active sessions (%d):\n", n);
      for (int i = 0; i < n; i++)
        printf("  [%d] %s\n", i, names[i]);
    }
    return 0;
  }

  const char *name = (argc > 1) ? argv[1] : "example";
  printf("Connecting to \"%s\" ...\n", name);

  texlink_client_desc_t desc = {
      .version = 1,
      .name = name,
      .backend = TEXLINK_BACKEND_CPU,
      .timeout_ms = 5000,
  };
  texlink_client_t *client = texlink_client_create(&desc);
  if (!client || texlink_client_connect(client) < 0) {
    fprintf(stderr, "Session \"%s\" not found or connection failed.\n", name);
    return 1;
  }
  printf("Connected.\n");

  texlink_meta_t meta = texlink_client_meta(client);
  printf("Buffer: %u bytes  format=0x%x\n", meta.size, meta.format);

  while (1) {
    int idx = texlink_client_acquire_frame(client);
    if (idx < 0) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    texlink_buf_t *buf = texlink_client_buf(client, idx);
    texlink_meta_t cur_meta = texlink_client_meta(client);
    uint64_t *data = texlink_buf_map_cpu(buf);
    if (data) {
      texlink_buf_cpu_begin(buf, 0);
      uint64_t val = data[0];
      texlink_buf_cpu_end(buf, 0);
      printf("frame=%" PRIu64 "  buf=%d  val=%" PRIu64 "\n", cur_meta.frame_id,
             idx, val);
    }

    texlink_client_release_frame(client, idx);
  }

  texlink_client_destroy(client);
  return 0;
}
