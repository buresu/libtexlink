#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static texlink_session_t *session_alloc(void) {
  texlink_session_t *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->sock_fd = TEXLINK_INVALID_SOCKET_HANDLE;
  s->shm_fd = TEXLINK_INVALID_OS_HANDLE;
  return s;
}

static void session_send_release(texlink_session_t *s, int idx);

static texlink_native_handle_type_t frame_handle_type_from_meta(
    const texlink_meta_t *meta) {
  texlink_native_handle_type_t type =
      meta ? (texlink_native_handle_type_t)meta->handle_type
           : TEXLINK_NATIVE_HANDLE_UNKNOWN;
  if (type != TEXLINK_NATIVE_HANDLE_UNKNOWN)
    return type;
  return texlink_default_native_handle_type();
}

static void session_free_consumer_frames(texlink_session_t *s) {
  for (int i = 0; i < s->buf_count; i++) {
    texlink_frame_t *frame = s->frames[i];
    if (!frame)
      continue;
    texlink_frame_unmap_ipc_view(frame);
    if (frame->sync_fd >= 0) {
      texlink_native_handle_t sync_handle = {
          .handle_type = TEXLINK_NATIVE_HANDLE_SYNC_FD,
          .owned = 1,
          .value.fd = frame->sync_fd,
      };
      texlink_native_handle_close(&sync_handle);
    }
    texlink_frame_close_ipc_handle(frame);
    texlink_frame_close_sync_handle(frame);
    free(frame);
    s->frames[i] = NULL;
  }
}

static void session_release_acquired_refs(texlink_session_t *s) {
  if (!s || s->is_producer)
    return;

  for (int i = 0; i < s->buf_count; i++) {
    while (s->acquired_refs[i] > 0) {
      session_send_release(s, i);
      s->acquired_refs[i]--;
    }
  }
}

static texlink_session_t *session_connect(const char *path) {
  texlink_socket_t sock_fd = texlink_socket_connect(path);
  if (sock_fd == TEXLINK_INVALID_SOCKET_HANDLE)
    return NULL;

  texlink_handshake_t hs;
  if (texlink_socket_recv(sock_fd, &hs, sizeof(hs)) != 0 ||
      hs.version != TEXLINK_PROTO_VER || hs.buf_count < 1 ||
      hs.buf_count > TEXLINK_MAX_BUFS) {
    texlink_socket_close(sock_fd);
    return NULL;
  }

  texlink_session_t *s = session_alloc();
  if (!s) {
    texlink_socket_close(sock_fd);
    return NULL;
  }

  s->sock_fd = sock_fd;
  s->buf_count = (int)hs.buf_count;
  s->buffering = hs.buffering;
  strncpy(s->shm_name, hs.shm_name, sizeof(s->shm_name) - 1);

  texlink_native_handle_type_t handle_type =
      frame_handle_type_from_meta(&hs.meta);
  texlink_native_handle_type_t sync_handle_type =
      (texlink_native_handle_type_t)hs.meta.sync_handle_type;
  if (!texlink_native_handle_type_is_ipc(handle_type))
    goto err;
  if (sync_handle_type != TEXLINK_NATIVE_HANDLE_UNKNOWN &&
      !texlink_native_handle_type_is_ipc(sync_handle_type))
    goto err;

  /* Receive native fd handles */
  for (int i = 0; i < s->buf_count; i++) {
    texlink_frame_t *frame = calloc(1, sizeof(*frame));
    if (!frame)
      goto err;
    frame->index = i;
    texlink_frame_init_received(frame);
    frame->meta = hs.meta;
    frame->size = hs.meta.size;
    s->frames[i] = frame;

    if (texlink_frame_recv_native_handle(sock_fd, frame, handle_type) < 0)
      goto err;
    if (sync_handle_type != TEXLINK_NATIVE_HANDLE_UNKNOWN &&
        texlink_frame_recv_sync_handle(sock_fd, frame, sync_handle_type) < 0)
      goto err;
  }

  /* Open shared memory created by producer */
  s->shm_fd = texlink_shm_open(s->shm_name);
  if (s->shm_fd == TEXLINK_INVALID_OS_HANDLE)
    goto err;

  s->shm = texlink_shm_map(s->shm_fd, sizeof(texlink_shm_t));
  if (texlink_shm_map_failed(s->shm))
    goto err;

  return s;

err:
  if (s->shm && !texlink_shm_map_failed(s->shm))
    texlink_shm_unmap(s->shm, sizeof(texlink_shm_t));
  texlink_shm_close(s->shm_fd);
  session_free_consumer_frames(s);
  texlink_socket_close(sock_fd);
  free(s);
  return NULL;
}

static void session_close(texlink_session_t *s) {
  if (!s)
    return;

  session_release_acquired_refs(s);

  if (s->is_registered) {
    texlink_registry_unregister(s->reg_name);
    s->is_registered = 0;
  }

  if (s->shm && !texlink_shm_map_failed(s->shm))
    texlink_shm_unmap(s->shm, sizeof(texlink_shm_t));
  if (s->shm_fd != TEXLINK_INVALID_OS_HANDLE) {
    texlink_shm_close(s->shm_fd);
    if (s->is_producer)
      texlink_shm_unlink(s->shm_name);
  }
  texlink_socket_close(s->sock_fd);

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

static void session_send_release(texlink_session_t *s, int idx) {
  if (!s || idx < 0 || idx >= s->buf_count)
    return;
  texlink_release_msg_t msg = {.buf_idx = (uint32_t)idx};
  (void)texlink_socket_send(s->sock_fd, &msg, sizeof(msg));
}

static texlink_frame_t *session_consumer_acquire(texlink_session_t *s) {
  if (!s || s->is_producer)
    return NULL;

  /* Block until producer sends a frame notification */
  int ret = texlink_socket_poll(s->sock_fd, 5000);
  if (ret <= 0)
    return NULL;

  texlink_frame_msg_t msg;
  int sync_fd;
  if (texlink_recv_frame(s->sock_fd, &msg, &sync_fd) < 0)
    return NULL;

  while (texlink_socket_poll(s->sock_fd, 0) > 0) {
    int old_idx = (int)msg.buf_idx;
    if (old_idx >= 0 && old_idx < s->buf_count)
      session_send_release(s, old_idx);
    if (sync_fd >= 0)
      texlink_close_sync_fd(sync_fd);

    if (texlink_recv_frame(s->sock_fd, &msg, &sync_fd) < 0)
      return NULL;
  }

  int idx = (int)msg.buf_idx;
  if (idx < 0 || idx >= s->buf_count) {
    if (sync_fd >= 0)
      texlink_close_sync_fd(sync_fd);
    return NULL;
  }

  /* Store frame_id from the socket message to avoid non-atomic shm read */
  s->last_frame_id = msg.frame_id;
  s->frames[idx]->meta.sync_value = msg.sync_value;

  /* acquire-load pairs with the release-store in producer_end */
  (void)atomic_load_explicit(&s->shm->current_idx, memory_order_acquire);

  /* Wait for GPU work on this frame to complete before any CPU access */
  if (sync_fd >= 0) {
    if (texlink_wait_sync_file(sync_fd, 5000) != 0) {
      texlink_close_sync_fd(sync_fd);
      session_send_release(s, idx);
      return NULL;
    }
    if (s->frames[idx]->sync_fd >= 0) {
      texlink_native_handle_t old_sync = {
          .handle_type = TEXLINK_NATIVE_HANDLE_SYNC_FD,
          .owned = 1,
          .value.fd = s->frames[idx]->sync_fd,
      };
      texlink_native_handle_close(&old_sync);
    }
    s->frames[idx]->sync_fd = sync_fd;
  }

  s->acquired_refs[idx]++;
  return s->frames[idx];
}

static void session_consumer_release(texlink_session_t *s,
                                     texlink_frame_t *frame) {
  int idx = session_frame_index(s, frame);
  if (!s || s->is_producer || idx < 0)
    return;
  if (s->acquired_refs[idx] == 0)
    return;

  session_send_release(s, idx);
  s->acquired_refs[idx]--;
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

  server->listen_fd = TEXLINK_INVALID_SOCKET_HANDLE;
  server->shm_fd = TEXLINK_INVALID_OS_HANDLE;
  server->shm = NULL;
  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++)
    server->client_fds[i] = TEXLINK_INVALID_SOCKET_HANDLE;
  server->frames = desc->frames;
  server->frame_count = desc->frame_count;
  for (uint32_t i = 0; i < server->frame_count; i++)
    server->frames[i]->index = (int)i;
  server->backend_type = desc->backend_type;
  server->state = TEXLINK_STATE_CREATED;

  if (desc->name)
    strncpy(server->name, desc->name, sizeof(server->name) - 1);
  if (desc->path)
    strncpy(server->path, desc->path, sizeof(server->path) - 1);
  else
    texlink_default_path_for_name(desc->name, server->path,
                                  sizeof(server->path));

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

  texlink_make_socket_dir(server->path);
  texlink_make_shm_name(server->path, server->shm_name,
                        sizeof(server->shm_name));
  texlink_shm_unlink(server->shm_name);

  server->listen_fd = texlink_socket_bind(server->path);
  if (server->listen_fd == TEXLINK_INVALID_SOCKET_HANDLE) {
    server->last_error = errno ? errno : EADDRINUSE;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  server->shm_fd = texlink_shm_create(server->shm_name, sizeof(texlink_shm_t));
  if (server->shm_fd == TEXLINK_INVALID_OS_HANDLE) {
    server->last_error = errno ? errno : EIO;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  server->shm = texlink_shm_map(server->shm_fd, sizeof(texlink_shm_t));
  if (texlink_shm_map_failed(server->shm)) {
    server->last_error = errno ? errno : EIO;
    server->state = TEXLINK_STATE_ERROR;
    return -1;
  }

  atomic_store(&server->shm->frame_id, 0);
  atomic_store(&server->shm->current_idx, 0);
  for (uint32_t i = 0; i < TEXLINK_MAX_BUFS; i++)
    atomic_store(&server->shm->frame_refs[i], 0);
  server->shm->buf_count = server->frame_count;
  server->shm->meta = server->frames[0]->meta;
  server->shm->meta.backend_type = (uint32_t)server->backend_type;
  if (server->shm->meta.handle_type == TEXLINK_NATIVE_HANDLE_UNKNOWN)
    server->shm->meta.handle_type =
        (uint32_t)texlink_default_native_handle_type();

  if (server->name[0] != '\0') {
    texlink_registry_announce(server->name, server->path);
    server->is_registered = 1;
  }

  (void)mode;
  server->state = TEXLINK_STATE_LISTENING;
  return 0;
}

static void server_drop_client(texlink_server_t *server, int slot);

static void server_handle_client_releases(texlink_server_t *server, int slot) {
  if (!server || slot < 0 || slot >= TEXLINK_MAX_CLIENTS)
    return;
  texlink_socket_t fd = server->client_fds[slot];
  if (fd == TEXLINK_INVALID_SOCKET_HANDLE)
    return;

  while (texlink_socket_poll(fd, 0) > 0) {
    texlink_release_msg_t msg;
    if (texlink_socket_recv(fd, &msg, sizeof(msg)) != 0) {
      server_drop_client(server, slot);
      return;
    }
    if (msg.buf_idx >= server->frame_count)
      continue;
    if (server->client_refs[slot][msg.buf_idx] == 0)
      continue;

    server->client_refs[slot][msg.buf_idx]--;
    atomic_fetch_sub_explicit(&server->shm->frame_refs[msg.buf_idx], 1,
                              memory_order_release);
  }
}

int texlink_server_poll(texlink_server_t *server) {
  if (!server)
    return -1;
  if (server->state == TEXLINK_STATE_ERROR ||
      server->listen_fd == TEXLINK_INVALID_SOCKET_HANDLE)
    return -1;

  int accepted = 0;
  while (1) {
    int ret = texlink_socket_poll(server->listen_fd, 0);
    if (ret <= 0)
      break;

    texlink_socket_t client_fd = TEXLINK_INVALID_SOCKET_HANDLE;
    if (texlink_socket_accept(server->listen_fd, &client_fd) < 0)
      break;

    int slot = -1;
    for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
      if (server->client_fds[i] == TEXLINK_INVALID_SOCKET_HANDLE) {
        slot = i;
        break;
      }
    }
    if (slot < 0) {
      texlink_socket_close(client_fd);
      continue;
    }

    texlink_handshake_t hs = {
        .version = TEXLINK_PROTO_VER,
        .buf_count = server->frame_count,
        .buffering = frame_count_to_mode(server->frame_count),
        .meta = server->shm->meta,
    };
    strncpy(hs.shm_name, server->shm_name, sizeof(hs.shm_name) - 1);

    if (texlink_socket_send(client_fd, &hs, sizeof(hs)) != 0) {
      texlink_socket_close(client_fd);
      continue;
    }

    int ok = 1;
    for (uint32_t i = 0; i < server->frame_count; i++) {
      texlink_native_handle_type_t handle_type =
          frame_handle_type_from_meta(&server->frames[i]->meta);
      if (texlink_frame_send_native_handle(client_fd, server->frames[i],
                                           handle_type) < 0) {
        ok = 0;
        break;
      }
      texlink_native_handle_type_t sync_handle_type =
          (texlink_native_handle_type_t)server->frames[i]->meta.sync_handle_type;
      if (sync_handle_type != TEXLINK_NATIVE_HANDLE_UNKNOWN &&
          texlink_frame_send_sync_handle(client_fd, server->frames[i],
                                         sync_handle_type) < 0) {
        ok = 0;
        break;
      }
    }
    if (!ok) {
      texlink_socket_close(client_fd);
      continue;
    }

    server->client_fds[slot] = client_fd;
    accepted++;
  }

  if (accepted > 0)
    server->state = TEXLINK_STATE_CONNECTED;

  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++)
    server_handle_client_releases(server, i);

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

static int server_frame_is_available(texlink_server_t *server, int idx,
                                     uint32_t current_idx) {
  if (!server || idx < 0 || (uint32_t)idx >= server->frame_count)
    return 0;
  if ((uint32_t)idx == current_idx)
    return 0;
  return atomic_load_explicit(&server->shm->frame_refs[idx],
                              memory_order_acquire) == 0;
}

static void server_drop_client(texlink_server_t *server, int slot) {
  if (!server || slot < 0 || slot >= TEXLINK_MAX_CLIENTS)
    return;

  texlink_socket_t fd = server->client_fds[slot];
  if (fd != TEXLINK_INVALID_SOCKET_HANDLE)
    texlink_socket_close(fd);
  server->client_fds[slot] = TEXLINK_INVALID_SOCKET_HANDLE;

  if (!server->shm || texlink_shm_map_failed(server->shm))
    return;

  for (uint32_t i = 0; i < server->frame_count; i++) {
    while (server->client_refs[slot][i] > 0) {
      atomic_fetch_sub_explicit(&server->shm->frame_refs[i], 1,
                                memory_order_release);
      server->client_refs[slot][i]--;
    }
  }
}

texlink_frame_t *texlink_server_begin_frame(texlink_server_t *server) {
  if (!server || !server->shm || texlink_shm_map_failed(server->shm))
    return NULL;

  uint32_t cur =
      atomic_load_explicit(&server->shm->current_idx, memory_order_acquire);

  switch (frame_count_to_mode(server->frame_count)) {
  case TEXLINK_BUFFERING_SINGLE:
    return atomic_load_explicit(&server->shm->frame_refs[0],
                                memory_order_acquire) == 0
               ? server->frames[0]
               : NULL;
  case TEXLINK_BUFFERING_DOUBLE: {
    int idx = 1 - (int)(cur & 1);
    return server_frame_is_available(server, idx, cur) ? server->frames[idx]
                                                       : NULL;
  }
  case TEXLINK_BUFFERING_TRIPLE: {
    for (uint32_t n = 0; n < server->frame_count; n++) {
      int idx = (server->write_idx + (int)n) % (int)server->frame_count;
      if (server_frame_is_available(server, idx, cur))
        return server->frames[idx];
    }
    return NULL;
  }
  default:
    return NULL;
  }
}

int texlink_server_end_frame(texlink_server_t *server, texlink_frame_t *frame) {
  int idx = server_frame_index(server, frame);
  if (!server || !server->shm || texlink_shm_map_failed(server->shm) ||
      idx < 0)
    return -1;

  int sync_fd = -1;
  if (server->frames[idx]->dma_fd >= 0)
    sync_fd = texlink_export_sync_file(server->frames[idx]->dma_fd);
  if (server->frames[idx]->sync_fd >= 0)
    texlink_close_sync_fd(server->frames[idx]->sync_fd);
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
      .sync_value = server->frames[idx]->meta.sync_value,
  };

  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
    texlink_socket_t fd = server->client_fds[i];
    if (fd == TEXLINK_INVALID_SOCKET_HANDLE)
      continue;
    atomic_fetch_add_explicit(&server->shm->frame_refs[idx], 1,
                              memory_order_acq_rel);
    server->client_refs[i][idx]++;
    if (texlink_send_frame(fd, &msg, sync_fd) < 0) {
      server_drop_client(server, i);
    }
  }
  return 0;
}

int texlink_server_client_count(texlink_server_t *server) {
  if (!server)
    return 0;
  int count = 0;
  for (int i = 0; i < TEXLINK_MAX_CLIENTS; i++) {
    if (server->client_fds[i] != TEXLINK_INVALID_SOCKET_HANDLE)
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
    server_drop_client(server, i);
  }
  texlink_socket_close(server->listen_fd);
  if (server->shm && !texlink_shm_map_failed(server->shm))
    texlink_shm_unmap(server->shm, sizeof(texlink_shm_t));
  if (server->shm_fd != TEXLINK_INVALID_OS_HANDLE) {
    texlink_shm_close(server->shm_fd);
    texlink_shm_unlink(server->shm_name);
  }
  texlink_cleanup_socket_path(server->path);
  server->state = TEXLINK_STATE_CLOSED;
  free(server);
}

texlink_client_t *texlink_client_create(const texlink_client_desc_t *desc) {
  if (!desc || (!desc->path && (!desc->name || desc->name[0] == '\0')))
    return NULL;

  texlink_client_t *client = calloc(1, sizeof(*client));
  if (!client)
    return NULL;

  client->backend_type = desc->backend_type;
  client->timeout_ms = desc->timeout_ms;
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

int texlink_client_frame_count(texlink_client_t *client) {
  if (!client || !client->session)
    return 0;
  return client->session->buf_count;
}

texlink_frame_t *texlink_client_frame(texlink_client_t *client, int idx) {
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
