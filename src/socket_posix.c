#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int texlink_send_ipc_handles(texlink_socket_t sock,
                             const texlink_ipc_handle_t *fds, int nfds) {
  char dummy = 0;
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};

  size_t cmsg_sz = CMSG_SPACE(sizeof(int) * (size_t)nfds);
  void *cmsg_buf = calloc(1, cmsg_sz);
  if (!cmsg_buf)
    return -1;

  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = cmsg_sz,
  };

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)nfds);
  memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)nfds);

  ssize_t ret = sendmsg(sock, &msg, MSG_NOSIGNAL);
  free(cmsg_buf);
  return (ret < 0) ? -1 : 0;
}

int texlink_recv_ipc_handles(texlink_socket_t sock, texlink_ipc_handle_t *fds,
                             int nfds) {
  char dummy;
  struct iovec iov = {.iov_base = &dummy, .iov_len = 1};

  size_t cmsg_sz = CMSG_SPACE(sizeof(int) * (size_t)nfds);
  void *cmsg_buf = calloc(1, cmsg_sz);
  if (!cmsg_buf)
    return -1;

  struct msghdr msg = {
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = cmsg_buf,
      .msg_controllen = cmsg_sz,
  };

  ssize_t ret = recvmsg(sock, &msg, MSG_WAITALL);
  if (ret < 0) {
    free(cmsg_buf);
    return -1;
  }

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) {
    free(cmsg_buf);
    return -1;
  }

  memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * (size_t)nfds);
  free(cmsg_buf);
  return 0;
}

texlink_socket_t texlink_socket_bind(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  unlink(path);

  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
      listen(fd, TEXLINK_MAX_CLIENTS) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

texlink_socket_t texlink_socket_connect(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/*
 * Send per-frame notification.
 * Step 1: send the frame header via send() (reliable MSG_WAITALL counterpart).
 * Step 2: if has_sync_fd, send the fence fd via a separate sendmsg/SCM_RIGHTS.
 *
 * Splitting into two syscalls avoids the MSG_WAITALL + recvmsg interaction
 * issue on SOCK_STREAM where ancillary data can interfere with byte counting.
 */
int texlink_socket_accept(texlink_socket_t server, texlink_socket_t *out_client) {
  int client = accept4(server, NULL, NULL, SOCK_CLOEXEC);
  if (client < 0)
    return -1;
  *out_client = client;
  return 0;
}

int texlink_socket_poll(texlink_socket_t sock, int timeout_ms) {
  struct pollfd pfd = {.fd = sock, .events = POLLIN};
  int ret = poll(&pfd, 1, timeout_ms);
  if (ret <= 0 || !(pfd.revents & POLLIN))
    return ret;
  return 1;
}

void texlink_socket_close(texlink_socket_t sock) {
  if (sock >= 0)
    close(sock);
}

int texlink_socket_send(texlink_socket_t sock, const void *data, size_t size) {
  ssize_t n = send(sock, data, size, MSG_NOSIGNAL);
  return n == (ssize_t)size ? 0 : -1;
}

int texlink_socket_recv(texlink_socket_t sock, void *data, size_t size) {
  ssize_t n = recv(sock, data, size, MSG_WAITALL);
  return n == (ssize_t)size ? 0 : -1;
}

int texlink_send_frame(texlink_socket_t sock, const texlink_frame_msg_t *msg,
                       int sync_fd) {
  if (texlink_socket_send(sock, msg, sizeof(*msg)) != 0)
    return -1;

  if (msg->has_sync_fd && sync_fd >= 0)
    return texlink_send_ipc_handles(sock, &sync_fd, 1);

  return 0;
}

/*
 * Receive per-frame notification. *sync_fd is set to -1 if no fence arrived.
 * Step 1: recv() the frame header (MSG_WAITALL guarantees full struct).
 * Step 2: if has_sync_fd, recv the fence fd via recvmsg/SCM_RIGHTS.
 */
int texlink_recv_frame(texlink_socket_t sock, texlink_frame_msg_t *msg,
                       int *sync_fd) {
  *sync_fd = -1;

  if (texlink_socket_recv(sock, msg, sizeof(*msg)) != 0)
    return -1;

  if (msg->has_sync_fd && texlink_recv_ipc_handles(sock, sync_fd, 1) != 0)
    return -1;

  return 0;
}
