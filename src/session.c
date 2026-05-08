#define _GNU_SOURCE
#include "dmabuflink_internal.h"

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
  snprintf(out, len, DMABL_SHM_PREFIX "%s", base);

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

static dmabl_session_t *session_alloc(void) {
  dmabl_session_t *s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->sock_fd = -1;
  s->shm_fd = -1;
  return s;
}

static void session_free_consumer_bufs(dmabl_session_t *s) {
  for (int i = 0; i < s->buf_count; i++) {
    dmabl_buf_t *b = s->bufs[i];
    if (!b)
      continue;
    if (b->map_ptr != MAP_FAILED && b->map_ptr)
      munmap(b->map_ptr, b->size);
    if (b->sync_fd >= 0)
      close(b->sync_fd);
    if (b->dma_fd >= 0)
      close(b->dma_fd);
    free(b);
    s->bufs[i] = NULL;
  }
}

static dmabl_session_t *serve_accept(int listen_fd, const char *path,
                                     dmabl_buf_t **bufs,
                                     dmabl_buffering_t buffering) {
  int buf_count = (int)buffering;

  dmabl_session_t *s = session_alloc();
  if (!s)
    return NULL;

  s->is_producer = 1;
  s->buf_count = buf_count;
  s->buffering = buffering;
  for (int i = 0; i < buf_count; i++)
    s->bufs[i] = bufs[i];

  strncpy(s->sock_path, path, sizeof(s->sock_path) - 1);
  make_shm_name(path, s->shm_name, sizeof(s->shm_name));
  shm_unlink(s->shm_name);

  s->shm_fd = shm_open(s->shm_name, O_CREAT | O_RDWR, 0600);
  if (s->shm_fd < 0)
    goto err;

  if (ftruncate(s->shm_fd, sizeof(dmabl_shm_t)) < 0)
    goto err;

  s->shm = mmap(NULL, sizeof(dmabl_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED,
                s->shm_fd, 0);
  if (s->shm == MAP_FAILED)
    goto err;

  atomic_store(&s->shm->frame_id, 0);
  atomic_store(&s->shm->current_idx, 0);
  s->shm->buf_count = (uint32_t)buf_count;
  s->shm->meta = bufs[0]->meta;

  int client_fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
  if (client_fd < 0)
    goto err;
  s->sock_fd = client_fd;

  dmabl_handshake_t hs = {
      .version = DMABL_PROTO_VER,
      .buf_count = (uint32_t)buf_count,
      .buffering = buffering,
      .meta = bufs[0]->meta,
  };
  strncpy(hs.shm_name, s->shm_name, sizeof(hs.shm_name) - 1);

  if (send(client_fd, &hs, sizeof(hs), MSG_NOSIGNAL) != sizeof(hs))
    goto err_sock;

  for (int i = 0; i < buf_count; i++) {
    if (dmabl_send_fds(client_fd, &bufs[i]->dma_fd, 1) < 0)
      goto err_sock;
  }

  return s;

err_sock:
  close(client_fd);
  s->sock_fd = -1;
err:
  if (s->shm && s->shm != MAP_FAILED)
    munmap(s->shm, sizeof(dmabl_shm_t));
  if (s->shm_fd >= 0) {
    close(s->shm_fd);
    shm_unlink(s->shm_name);
  }
  free(s);
  return NULL;
}

dmabl_session_t *dmabl_serve(const char *path, dmabl_buf_t **bufs,
                             dmabl_buffering_t buffering) {
  if ((int)buffering < 1 || (int)buffering > DMABL_MAX_BUFS || !bufs)
    return NULL;

  make_socket_dir(path);

  int listen_fd = dmabl_socket_bind(path);
  if (listen_fd < 0)
    return NULL;

  dmabl_session_t *s = serve_accept(listen_fd, path, bufs, buffering);
  close(listen_fd);
  return s;
}

/*
 * Named serve: bind → register in global registry → accept.
 * This allows consumers to discover the session by name before connecting.
 */
dmabl_session_t *dmabl_serve_named(const char *name, dmabl_buf_t **bufs,
                                   dmabl_buffering_t buffering) {
  if (!name || name[0] == '\0')
    return NULL;
  if ((int)buffering < 1 || (int)buffering > DMABL_MAX_BUFS || !bufs)
    return NULL;

  char path[128];
  snprintf(path, sizeof(path), "/tmp/dmabl/%s.sock", name);
  make_socket_dir(path);

  int listen_fd = dmabl_socket_bind(path);
  if (listen_fd < 0)
    return NULL;

  /* Register name → socket path BEFORE blocking on accept,
   * so consumers can discover and connect while we wait. */
  dmabl_registry_announce(name, path);

  dmabl_session_t *s = serve_accept(listen_fd, path, bufs, buffering);
  close(listen_fd);

  if (s)
    s->is_registered = 1; /* already announced; mark for unregister on close */

  return s;
}

dmabl_session_t *dmabl_connect(const char *path) {
  int sock_fd = dmabl_socket_connect(path);
  if (sock_fd < 0)
    return NULL;

  dmabl_handshake_t hs;
  if (recv(sock_fd, &hs, sizeof(hs), MSG_WAITALL) != (ssize_t)sizeof(hs) ||
      hs.version != DMABL_PROTO_VER || hs.buf_count < 1 ||
      hs.buf_count > DMABL_MAX_BUFS) {
    close(sock_fd);
    return NULL;
  }

  dmabl_session_t *s = session_alloc();
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
    dmabl_buf_t *b = calloc(1, sizeof(*b));
    if (!b)
      goto err;
    b->dma_fd = -1;
    b->sync_fd = -1;
    b->map_ptr = MAP_FAILED;
    b->drm_fd = -1;
    b->meta = hs.meta;
    b->size = hs.meta.size;
    s->bufs[i] = b;

    if (dmabl_recv_fds(sock_fd, &b->dma_fd, 1) < 0)
      goto err;
  }

  /* Open shared memory created by producer */
  s->shm_fd = shm_open(s->shm_name, O_RDWR, 0);
  if (s->shm_fd < 0)
    goto err;

  s->shm = mmap(NULL, sizeof(dmabl_shm_t), PROT_READ | PROT_WRITE, MAP_SHARED,
                s->shm_fd, 0);
  if (s->shm == MAP_FAILED)
    goto err;

  return s;

err:
  if (s->shm && s->shm != MAP_FAILED)
    munmap(s->shm, sizeof(dmabl_shm_t));
  if (s->shm_fd >= 0)
    close(s->shm_fd);
  session_free_consumer_bufs(s);
  close(sock_fd);
  free(s);
  return NULL;
}

void dmabl_session_close(dmabl_session_t *s) {
  if (!s)
    return;

  dmabl_unregister(s);

  if (s->shm && s->shm != MAP_FAILED)
    munmap(s->shm, sizeof(dmabl_shm_t));
  if (s->shm_fd >= 0) {
    close(s->shm_fd);
    if (s->is_producer)
      shm_unlink(s->shm_name);
  }
  if (s->sock_fd >= 0)
    close(s->sock_fd);

  /* Consumer owns the received fd duplicates */
  if (!s->is_producer)
    session_free_consumer_bufs(s);

  free(s);
}

dmabl_buf_t *dmabl_session_buf(dmabl_session_t *s, int idx) {
  if (!s || idx < 0 || idx >= s->buf_count)
    return NULL;
  return s->bufs[idx];
}

dmabl_meta_t dmabl_session_meta(dmabl_session_t *s) {
  if (!s || !s->shm) {
    dmabl_meta_t zero = {0};
    return zero;
  }
  dmabl_meta_t m = s->shm->meta;
  /* Use the frame_id received via socket message (authoritative, ordered) */
  m.frame_id = s->last_frame_id;
  return m;
}

int dmabl_producer_begin(dmabl_session_t *s) {
  if (!s || !s->is_producer)
    return -1;

  int idx;
  switch (s->buffering) {
  case DMABL_BUFFERING_SINGLE:
    idx = 0;
    break;
  case DMABL_BUFFERING_DOUBLE: {
    /* Write to the buffer the consumer is NOT currently reading */
    uint32_t cur = atomic_load(&s->shm->current_idx);
    idx = 1 - (int)(cur & 1);
    break;
  }
  case DMABL_BUFFERING_TRIPLE: {
    /* Pick a slot that the consumer is NOT currently reading */
    uint32_t cur = atomic_load(&s->shm->current_idx);
    idx = s->write_idx;
    if ((uint32_t)idx == cur)
      idx = (idx + 1) % 3;
    break;
  }
  default:
    return -1;
  }

  return idx;
}

void dmabl_producer_end(dmabl_session_t *s, int idx) {
  if (!s || !s->is_producer || idx < 0 || idx >= s->buf_count)
    return;

  /*
   * Export the GPU fence BEFORE updating shm so that the fence captures
   * all GPU work submitted to this buffer in this frame.
   */
  int sync_fd = dmabl_export_sync_file(s->bufs[idx]->dma_fd);
  if (s->bufs[idx]->sync_fd >= 0)
    close(s->bufs[idx]->sync_fd);
  s->bufs[idx]->sync_fd = sync_fd;

  /* Commit to shared memory — store_release semantics via atomic_store */
  uint64_t frame_id = atomic_fetch_add(&s->shm->frame_id, 1) + 1;
  s->shm->meta.frame_id = frame_id;
  atomic_store_explicit(&s->shm->current_idx, (uint32_t)idx,
                        memory_order_release);

  /* Advance triple-buffer write cursor for next producer_begin() */
  if (s->buffering == DMABL_BUFFERING_TRIPLE)
    s->write_idx = (idx + 1) % 3;

  /* Notify consumer via socket (data + optional SCM_RIGHTS fence) */
  dmabl_frame_msg_t msg = {
      .frame_id = frame_id,
      .buf_idx = (uint32_t)idx,
      .has_sync_fd = (sync_fd >= 0) ? 1 : 0,
  };
  dmabl_send_frame(s->sock_fd, &msg, sync_fd);
}

int dmabl_consumer_acquire(dmabl_session_t *s) {
  if (!s || s->is_producer)
    return -1;

  /* Block until producer sends a frame notification */
  struct pollfd pfd = {.fd = s->sock_fd, .events = POLLIN};
  int ret = poll(&pfd, 1, 5000);
  if (ret <= 0)
    return -1;

  dmabl_frame_msg_t msg;
  int sync_fd;
  if (dmabl_recv_frame(s->sock_fd, &msg, &sync_fd) < 0)
    return -1;

  int idx = (int)msg.buf_idx;
  if (idx < 0 || idx >= s->buf_count)
    return -1;

  /* Store frame_id from the socket message — avoids non-atomic shm read */
  s->last_frame_id = msg.frame_id;

  /* acquire-load pairs with the release-store in producer_end */
  (void)atomic_load_explicit(&s->shm->current_idx, memory_order_acquire);

  /* Wait for GPU work on this buffer to complete before any CPU access */
  if (sync_fd >= 0) {
    dmabl_wait_sync_file(sync_fd, 5000);
    if (s->bufs[idx]->sync_fd >= 0)
      close(s->bufs[idx]->sync_fd);
    s->bufs[idx]->sync_fd = sync_fd;
  }

  return idx;
}

void dmabl_consumer_release(dmabl_session_t *s, int idx) {
  /* No-op for now: buffers are read-only on the consumer side */
  (void)s;
  (void)idx;
}
