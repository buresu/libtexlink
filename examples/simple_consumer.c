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
  setvbuf(stdout, NULL, _IONBF, 0);

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
      .name = name,
      .backend_type = TEXLINK_BACKEND_CPU,
      .timeout_ms = 5000,
  };
  texlink_client_t *client = texlink_client_create(&desc);
  if (!client || texlink_client_connect(client) < 0) {
    fprintf(stderr, "Session \"%s\" not found or connection failed.\n", name);
    return 1;
  }
  printf("Connected.\n");

  texlink_meta_t meta = texlink_client_meta(client);
  printf("Frame: %llu bytes  format=0x%x\n", (unsigned long long)meta.size, meta.format);

  int frame_count = texlink_client_frame_count(client);
  texlink_mapping_t mappings[3] = {0};
  texlink_map_desc_t map_desc = {
      .flags = TEXLINK_MAP_READ,
      .offset = 0,
      .size = 0,
  };
  for (int i = 0; i < frame_count; i++) {
    texlink_frame_t *frame = texlink_client_frame(client, i);
    if (texlink_frame_map(frame, &map_desc, &mappings[i]) != 0) {
      fprintf(stderr, "texlink_frame_map failed\n");
      texlink_client_destroy(client);
      return 1;
    }
  }

  while (1) {
    texlink_frame_t *frame = texlink_client_acquire_frame(client);
    if (!frame) {
      fprintf(stderr, "Acquire failed (producer disconnected?)\n");
      break;
    }

    texlink_meta_t cur_meta = texlink_client_meta(client);
    texlink_map_desc_t access_desc = {
        .flags = TEXLINK_MAP_READ,
        .offset = 0,
        .size = 0,
    };
    if (texlink_frame_cpu_begin(frame, &access_desc) == 0) {
      uint64_t *data = mappings[texlink_frame_index(frame)].data;
      uint64_t val = data[0];
      texlink_frame_cpu_end(frame, &access_desc);
      printf("frame=%" PRIu64 "  slot=%d  val=%" PRIu64 "\n", cur_meta.frame_id,
             texlink_frame_index(frame), val);
    }

    texlink_client_release_frame(client, frame);
  }

  for (int i = 0; i < frame_count; i++)
    texlink_frame_unmap(texlink_client_frame(client, i));
  texlink_client_destroy(client);
  return 0;
}
