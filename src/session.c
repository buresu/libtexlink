#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_shm_name(const char *path, char *out, size_t len) {
  const char *base = strrchr(path, '/');
  base = base ? base + 1 : path;
  snprintf(out, len, TEXLINK_SHM_PREFIX "%s", base);

  /* Strip common suffixes */
  char *dot = strrchr(out, '.');
  if (dot)
    *dot = '\0';
}

static void make_socket_dir(const char *path) {
  char dir[256];
  snprintf(dir, sizeof(dir), "%s", path);
  char *slash = strrchr(dir, '/');
  if (slash) {
    *slash = '\0';
    mkdir(dir, 0755);
  }
}

static texlink_session_t *session_alloc(void) {
  texlink_session_t *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->sock_fd = -1;
  s->shm_fd = -1;
  return s;
}

static void session_free_consumer_frames(texlink_session_t *s) {
  for (int i = 0; i < s->buf_count; i++) {
    texlink_frame_t *frame = s->frames[i];
    if (!frame)
      continue;
    if (frame->map_base != MAP_FAILED && frame->map_base)
      munmap(frame->map_base, frame->map_size);
    if (frame->sync_fd >= 0)
      close(frame->sync_fd);
    if (frame->dma_fd >= 0)
      close(frame->dma_fd);
    free(frame);
    s->frames[i] = NULL;
  }
}

static texlink_session_t *session_connect(const char *path) {
  int sock_fd = texlink_socket_connect(path);
  if (sock_fd < 0)
    return NULL;

  texlink_handshake_t hs;
  if (recv(sock_fd, &hs, sizeof(hs), MSG_WAITALL) != (ssize_t)sizeof(hs) ||
      hs.version != TEXLINK_PROTO_VER || hs.buf_count < 1 ||
      hs.buf_count > TEXLINK_MAX_BUFS) {
    close(sock_fd);
    return NULL;
  }

  texlink_session_t *s = session_alloc();
  if (!s) {
    close(sock_fd);
    return NULL;
  }

  s->sock_fd = sock_fd;
  s->buf_count = (int)hs.buf_count;
  s->buffering = hs.buffering;
  strncpy(s->shm_name, hs.shm_name, sizeof(s->shm_name) - 1);

  /* Receive dma_fds */
  for (int i = 0; i < s->buf_count; i++) {
    texlink_frame_t *frame = calloc(1, sizeof(*frame));
    if (!frame)
      goto err;
    frame->dma_fd = -1;
    frame->sync_fd = -1;
    frame->index = i;
    frame->map_base = MAP_FAILED;
    frame->map_ptr = MAP_FAILED;
    frame->drm_fd = -1;
    frame->meta = hs.meta;
    frame->size = hs.meta.size;
    s->frames[i] = frame;

    if (texlink_recv_fds(sock_fd, &frame->dma_fd, 1) < 0)
      goto err;
  }

  /* Open shared memory created by producer */
  s->shm_fd = shm_open(s->shm_name, O_RDWR, 0);
  if (s->shm_fd < 0)
    goto err;

  s->shm = mmap(NULL, sizeof(texlink_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED,
                s->shm_fd, 0);
  if (s->shm == MAP_FAILED)
    goto err;

  return s;

err:
  if (s->shm && s->shm != MAP_FAILED)
    munmap(s->shm, sizeof(texlink_shm_t));
  if (s->shm_fd >= 0)
    close(s->shm_fd);
  session_free_consumer_frames(s);
  close(sock_fd);
  free(s);
  return NULL;
}

static void session_close(texlink_session_t *s) {
  if (!s)
    return;

  if (s->is_registered) {
    texlink_registry_unregister(s->reg_name);
    s->is_registered = 0;
  }

  if (s->shm && s->shm != MAP_FAILED)
    munmap(s->shm, sizeof(texlink_shm_t));
  if (s->shm_fd >= 0) {
    close(s->shm_fd);
    if (s->is_producer)
      shm_unlink(s->shm_name);
  }
  if (s->sock_fd >= 0)
    close(s->sock_fd);

  /* Consumer owns the received fd duplicates */
  if (!s->is_producer)
    session_free_consumer_frames(s);

  free(s);
}

static texlink_frame_t *session_frame(texlink_session_t *s, int idx) {
  if (!s || idx < 0 || idx >= s->buf_count)
    return NULL;
  return s->frames[idx];
}

static int session_frame_index(texlink_session_t *s, texlink_frame_t *frame) {
  if (!s || !frame)
    return -1;
  for (int i = 0; i < s->buf_count; i++) {
    if (s->frames[i] == frame)
      return i;
  }
  return -1;
}

static texlink_meta_t session_meta(texlink_session_t *s) {
  if (!s || !s->shm) {
    texlink_meta_t zero = {0};
    return zero;
  }
  texlink_meta_t m = s->shm->meta;
  /* Use the frame_id received via socket message (authoritative, ordered) */
  m.frame_id = s->last_frame_id;
  return m;
}

static texlink_frame_t *session_consumer_acquire(texlink_session_t *s) {
  if (!s || s->is_producer)
    return NULL;

  /* Block until producer sends a frame notification */
  struct pollfd pfd = {.fd = s->sock_fd, .events = POLLIN};
  int ret = poll(&pfd, 1, 5000);
  if (ret <= 0)
    return NULL;

  texlink_frame_msg_t msg;
  int sync_fd;
  if (texlink_recv_frame(s->sock_fd, &msg, &sync_fd) < 0)
    return NULL;

  int idx = (int)msg.buf_idx;
  if (idx < 0 || idx >= s->buf_count)
    return NULL;

  /* Store frame_id from the socket message — avoids non-atomic shm read */
  s->last_frame_id = msg.frame_id;

  /* acquire-load pairs with the release-store in producer_end */
  (void)atomic_load_explicit(&s->shm->current_idx, memory_order_acquire);

  /* Wait for GPU work on this frame to complete before any CPU access */
  if (sync_fd >= 0) {
    texlink_wait_sync_file(sync_fd, 5000);
    if (s->frames[idx]->sync_fd >= 0)
      close(s->frames[idx]->sync_fd);
    s->frames[idx]->sync_fd = sync_fd;
  }

  return s->frames[idx];
}

static void session_consumer_release(texlink_session_t *s,
                                     texlink_frame_t *frame) {
  /* No-op for now: frames are read-only on the consumer side */
  (void)session_frame_index(s, frame);
}

static void default_path_for_name(const char *name, char *path, size_t len) {
  snprintf(path, len, "/tmp/texlink/%s.sock", name);
}

static texlink_buffering_t frame_count_to_mode(uint32_t count) {
  switch (count) {
  case 1:
    return TEXLINK_BUFFERING_SINGLE;
  case 2:
    return TEXLINK_BUFFERING_DOUBLE;
  case 3:
    return TEXLINK_BUFFERING_TRIPLE;
  default:
    return 0;
  }
}

texlink_server_t *texlink_server_create(const texlink_server_desc_t *desc) {
  if (!desc || !desc->frames || desc->frame_count == 0 ||
      desc->frame_count > TEXLINK_MAX_BUFS) {
    return NULL;
  }
  if (!desc->path && (!desc->name || desc->name[0] == '\0'))
    return NULL;

  texlink_server_t *server = calloc(1, sizeof(*server));
  if (!server)
    return NULL;

  server->listen_fd = -1;
  server->shm_fd = -1;
  server->shm = MAP_FAILED;
  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++)
    server->client_fds[i] = -1;
  server->frames = desc->frames;
  server->frame_count = desc->frame_count;
  for (uint32_t i = 0; i < server->frame_count; i++)
    server->frames[i]->index = (int)i;
  server->backend = desc->backend;
  server->flags = desc->flags;
  server->state = TEXLINK_STATE_CREATED;

  if (desc->name)
    strncpy(server->name, desc->name, sizeof(server->name) - 1);
  if (desc->path)
    strncpy(server->path, desc->path, sizeof(server->path) - 1);
  else
    default_path_for_name(desc->name, server->path, sizeof(server->path));

  return server;
}

int texlink_server_start(texlink_server_t *server) {
  if (!server || server->state != TEXLINK_STATE_CREATED)
    return -1;

  texlink_buffering_t mode = frame_count_to_mode(server->frame_count);
  if (!mode) {
    server->last_error = EINVAL;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  make_socket_dir(server->path);
  make_shm_name(server->path, server->shm_name, sizeof(server->shm_name));
  shm_unlink(server->shm_name);

  server->listen_fd = texlink_socket_bind(server->path);
  if (server->listen_fd < 0) {
    server->last_error = errno ? errno : EADDRINUSE;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  server->shm_fd = shm_open(server->shm_name, O_CREAT | O_RDWR, 0600);
  if (server->shm_fd < 0 ||
      ftruncate(server->shm_fd, sizeof(texlink_shm_t)) < 0) {
    server->last_error = errno ? errno : EIO;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  server->shm = mmap(NULL, sizeof(texlink_shm_t), PROT_READ | PROT_WRITE,
                     MAP_SHARED, server->shm_fd, 0);
  if (server->shm == MAP_FAILED) {
    server->last_error = errno ? errno : EIO;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  atomic_store(&server->shm->frame_id, 0);
  atomic_store(&server->shm->current_idx, 0);
  server->shm->buf_count = server->frame_count;
  server->shm->meta = server->frames[0]->meta;
  server->shm->meta.backend = (uint32_t)server->backend;

  if (server->name[0] != '\0') {
    texlink_registry_announce(server->name, server->path);
    server->is_registered = 1;
  }

  (void)mode;
  server->state = TEXLINK_STATE_LISTENING;
  return 0;
}

int texlink_server_poll(texlink_server_t *server) {
  if (!server)
    return -1;
  if (server->state == TEXLINK_STATE_ERROR || server->listen_fd < 0)
    return -1;

  int accepted = 0;
  while (1) {
    struct pollfd pfd = {.fd = server->listen_fd, .events = POLLIN};
    int ret = poll(&pfd, 1, 0);
    if (ret <= 0 || !(pfd.revents & POLLIN))
      break;

    int client_fd = accept4(server->listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client_fd < 0)
      break;

    int slot = -1;
    for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
      if (server->client_fds[i] < 0) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      close(client_fd);
      continue;
    }

    texlink_handshake_t hs = {
        .version = TEXLINK_PROTO_VER,
        .buf_count = server->frame_count,
        .buffering = frame_count_to_mode(server->frame_count),
        .meta = server->shm->meta,
    };
    strncpy(hs.shm_name, server->shm_name, sizeof(hs.shm_name) - 1);

    if (send(client_fd, &hs, sizeof(hs), MSG_NOSIGNAL) != sizeof(hs)) {
      close(client_fd);
      continue;
    }

    int ok = 1;
    for (uint32_t i = 0; i < server->frame_count; i++) {
      if (texlink_send_fds(client_fd, &server->frames[i]->dma_fd, 1) < 0) {
        ok = 0;
        break;
      }
    }
    if (!ok) {
      close(client_fd);
      continue;
    }

    server->client_fds[slot] = client_fd;
    accepted++;
  }

  if (accepted > 0)
    server->state = TEXLINK_STATE_CONNECTED;
  return accepted;
}

static int server_frame_index(texlink_server_t *server,
                              texlink_frame_t *frame) {
  if (!server || !frame)
    return -1;
  for (uint32_t i = 0; i < server->frame_count; i++) {
    if (server->frames[i] == frame)
      return (int)i;
  }
  return -1;
}

texlink_frame_t *texlink_server_begin_frame(texlink_server_t *server) {
  if (!server || !server->shm || server->shm == MAP_FAILED)
    return NULL;

  switch (frame_count_to_mode(server->frame_count)) {
  case TEXLINK_BUFFERING_SINGLE:
    return server->frames[0];
  case TEXLINK_BUFFERING_DOUBLE: {
    uint32_t cur = atomic_load(&server->shm->current_idx);
    return server->frames[1 - (int)(cur & 1)];
  }
  case TEXLINK_BUFFERING_TRIPLE: {
    uint32_t cur = atomic_load(&server->shm->current_idx);
    int idx = server->write_idx;
    if ((uint32_t)idx == cur)
      idx = (idx + 1) % 3;
    return server->frames[idx];
  }
  default:
    return NULL;
  }
}

int texlink_server_end_frame(texlink_server_t *server, texlink_frame_t *frame) {
  int idx = server_frame_index(server, frame);
  if (!server || !server->shm || server->shm == MAP_FAILED || idx < 0)
    return -1;

  int sync_fd = texlink_export_sync_file(server->frames[idx]->dma_fd);
  if (server->frames[idx]->sync_fd >= 0)
    close(server->frames[idx]->sync_fd);
  server->frames[idx]->sync_fd = sync_fd;

  uint64_t frame_id = atomic_fetch_add(&server->shm->frame_id, 1) + 1;
  server->shm->meta.frame_id = frame_id;
  atomic_store_explicit(&server->shm->current_idx, (uint32_t)idx,
                        memory_order_release);

  if (server->frame_count == 3)
    server->write_idx = (idx + 1) % 3;

  texlink_frame_msg_t msg = {
      .frame_id = frame_id,
      .buf_idx = (uint32_t)idx,
      .has_sync_fd = (sync_fd >= 0) ? 1 : 0,
  };

  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
    int fd = server->client_fds[i];
    if (fd < 0)
      continue;
    if (texlink_send_frame(fd, &msg, sync_fd) < 0) {
      close(fd);
      server->client_fds[i] = -1;
    }
  }
  return 0;
}

int texlink_server_client_count(texlink_server_t *server) {
  if (!server)
    return 0;
  int count = 0;
  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
    if (server->client_fds[i] >= 0)
      count++;
  }
  return count;
}

void texlink_server_destroy(texlink_server_t *server) {
  if (!server)
    return;
  if (server->is_registered)
    texlink_registry_unregister(server->name);
  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
    if (server->client_fds[i] >= 0)
      close(server->client_fds[i]);
  }
  if (server->listen_fd >= 0)
    close(server->listen_fd);
  if (server->shm && server->shm != MAP_FAILED)
    munmap(server->shm, sizeof(texlink_shm_t));
  if (server->shm_fd >= 0) {
    close(server->shm_fd);
    shm_unlink(server->shm_name);
  }
  if (server->path[0] != '\0')
    unlink(server->path);
  server->state = TEXLINK_STATE_CLOSED;
  free(server);
}

texlink_client_t *texlink_client_create(const texlink_client_desc_t *desc) {
  if (!desc || (!desc->path && (!desc->name || desc->name[0] == '\0')))
    return NULL;

  texlink_client_t *client = calloc(1, sizeof(*client));
  if (!client)
    return NULL;

  client->backend = desc->backend;
  client->timeout_ms = desc->timeout_ms;
  client->flags = desc->flags;
  client->state = TEXLINK_STATE_CREATED;

  if (desc->name)
    strncpy(client->name, desc->name, sizeof(client->name) - 1);
  if (desc->path)
    strncpy(client->path, desc->path, sizeof(client->path) - 1);

  return client;
}

int texlink_client_connect(texlink_client_t *client) {
  if (!client || client->state != TEXLINK_STATE_CREATED)
    return -1;

  client->state = TEXLINK_STATE_CONNECTING;
  if (client->path[0] == '\0' &&
      texlink_registry_resolve(client->name, client->path,
                               sizeof(client->path)) < 0) {
    client->last_error = ENOENT;
    client->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  client->session = session_connect(client->path);
  if (!client->session) {
    client->last_error = errno ? errno : ECONNREFUSED;
    client->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  client->state = TEXLINK_STATE_CONNECTED;
  return 0;
}

void texlink_client_disconnect(texlink_client_t *client) {
  if (!client)
    return;
  session_close(client->session);
  client->session = NULL;
  client->state = TEXLINK_STATE_DISCONNECTED;
}

void texlink_client_destroy(texlink_client_t *client) {
  if (!client)
    return;
  texlink_client_disconnect(client);
  client->state = TEXLINK_STATE_CLOSED;
  free(client);
}

texlink_frame_t *texlink_client_acquire_frame(texlink_client_t *client) {
  if (!client || !client->session)
    return NULL;
  return session_consumer_acquire(client->session);
}

void texlink_client_release_frame(texlink_client_t *client,
                                  texlink_frame_t *frame) {
  if (!client || !client->session)
    return;
  session_consumer_release(client->session, frame);
}

uint32_t texlink_client_frame_count(texlink_client_t *client) {
  if (!client || !client->session)
    return 0;
  return (uint32_t)client->session->buf_count;
}

texlink_frame_t *texlink_client_frame(texlink_client_t *client, uint32_t idx) {
  if (!client || !client->session)
    return NULL;
  return session_frame(client->session, (int)idx);
}

texlink_meta_t texlink_client_meta(texlink_client_t *client) {
  if (!client || !client->session) {
    texlink_meta_t zero = {0};
    return zero;
  }
  return session_meta(client->session);
}

texlink_state_t texlink_server_state(texlink_server_t *server) {
  return server ? server->state : TEXLINK_STATE_CLOSED;
}

texlink_state_t texlink_client_state(texlink_client_t *client) {
  return client ? client->state : TEXLINK_STATE_CLOSED;
}

int texlink_server_last_error(texlink_server_t *server) {
  return server ? server->last_error : EINVAL;
}

int texlink_client_last_error(texlink_client_t *client) {
  return client ? client->last_error : EINVAL;
}

const char *texlink_error_string(int err) {
  if (err == 0)
    return "no error";
  return strerror(err);
}
