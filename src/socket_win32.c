#include "texlink_internal.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  TEXLINK_WIN32_SOCKET_SERVER,
  TEXLINK_WIN32_SOCKET_PIPE,
} texlink_win32_socket_kind_t;

struct texlink_win32_socket {
  texlink_win32_socket_kind_t kind;
  HANDLE pipe;
  char path[TEXLINK_SOCKET_PATH_MAX];
};

static HANDLE create_pipe_instance(const char *path) {
  SECURITY_DESCRIPTOR sd;
  SECURITY_ATTRIBUTES sa;
  InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = &sd;
  sa.bInheritHandle = FALSE;

  return CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX,
                          PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
                          TEXLINK_MAX_CLIENTS, 65536, 65536, 0, &sa);
}

static texlink_socket_t socket_alloc(texlink_win32_socket_kind_t kind,
                                     HANDLE pipe, const char *path) {
  texlink_socket_t s = calloc(1, sizeof(*s));
  if (!s)
    return NULL;
  s->kind = kind;
  s->pipe = pipe;
  if (path)
    strncpy(s->path, path, sizeof(s->path) - 1);
  return s;
}

texlink_socket_t texlink_socket_bind(const char *path) {
  HANDLE pipe = create_pipe_instance(path);
  if (pipe == INVALID_HANDLE_VALUE)
    return TEXLINK_INVALID_SOCKET_HANDLE;
  return socket_alloc(TEXLINK_WIN32_SOCKET_SERVER, pipe, path);
}

texlink_socket_t texlink_socket_connect(const char *path) {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  DWORD start = GetTickCount();
  do {
    pipe = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (pipe != INVALID_HANDLE_VALUE)
      break;
    if (GetLastError() == ERROR_PIPE_BUSY)
      WaitNamedPipeA(path, 50);
    else
      Sleep(1);
  } while (GetTickCount() - start < 5000);

  if (pipe == INVALID_HANDLE_VALUE)
    return TEXLINK_INVALID_SOCKET_HANDLE;

  DWORD mode = PIPE_READMODE_BYTE | PIPE_WAIT;
  SetNamedPipeHandleState(pipe, &mode, NULL, NULL);
  return socket_alloc(TEXLINK_WIN32_SOCKET_PIPE, pipe, path);
}

int texlink_socket_poll(texlink_socket_t sock, int timeout_ms) {
  if (!sock || sock->pipe == INVALID_HANDLE_VALUE)
    return -1;

  if (sock->kind == TEXLINK_WIN32_SOCKET_SERVER) {
    BOOL ok = ConnectNamedPipe(sock->pipe, NULL);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (ok || err == ERROR_PIPE_CONNECTED)
      return 1;
    if (err == ERROR_PIPE_LISTENING)
      return 0;
    return -1;
  }

  DWORD available = 0;
  DWORD start = GetTickCount();
  do {
    if (!PeekNamedPipe(sock->pipe, NULL, 0, NULL, &available, NULL))
      return -1;
    if (available > 0)
      return 1;
    if (timeout_ms == 0)
      return 0;
    Sleep(1);
  } while ((int)(GetTickCount() - start) < timeout_ms);
  return 0;
}

int texlink_socket_accept(texlink_socket_t server, texlink_socket_t *out_client) {
  if (!server || server->kind != TEXLINK_WIN32_SOCKET_SERVER || !out_client)
    return -1;
  if (texlink_socket_poll(server, 0) <= 0)
    return -1;

  HANDLE client_pipe = server->pipe;
  server->pipe = create_pipe_instance(server->path);
  if (server->pipe == INVALID_HANDLE_VALUE) {
    CloseHandle(client_pipe);
    return -1;
  }

  *out_client = socket_alloc(TEXLINK_WIN32_SOCKET_PIPE, client_pipe,
                             server->path);
  return *out_client ? 0 : -1;
}

void texlink_socket_close(texlink_socket_t sock) {
  if (!sock)
    return;
  if (sock->pipe && sock->pipe != INVALID_HANDLE_VALUE) {
    if (sock->kind == TEXLINK_WIN32_SOCKET_PIPE)
      FlushFileBuffers(sock->pipe);
    DisconnectNamedPipe(sock->pipe);
    CloseHandle(sock->pipe);
  }
  free(sock);
}

int texlink_socket_send(texlink_socket_t sock, const void *data, size_t size) {
  if (!sock || !data)
    return -1;
  const uint8_t *p = data;
  size_t written_total = 0;
  while (written_total < size) {
    DWORD chunk = 0;
    DWORD want = (DWORD)(size - written_total);
    if (!WriteFile(sock->pipe, p + written_total, want, &chunk, NULL))
      return -1;
    written_total += chunk;
  }
  return 0;
}

int texlink_socket_recv(texlink_socket_t sock, void *data, size_t size) {
  if (!sock || !data)
    return -1;
  uint8_t *p = data;
  size_t read_total = 0;
  while (read_total < size) {
    DWORD chunk = 0;
    DWORD want = (DWORD)(size - read_total);
    if (!ReadFile(sock->pipe, p + read_total, want, &chunk, NULL) || chunk == 0)
      return -1;
    read_total += chunk;
  }
  return 0;
}

int texlink_send_fds(texlink_socket_t sock, const int *fds, int nfds) {
  if (!sock || !fds || nfds != 1)
    return -1;

  ULONG client_pid = 0;
  if (!GetNamedPipeClientProcessId(sock->pipe, &client_pid))
    return -1;

  HANDLE client_proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
  if (!client_proc)
    return -1;

  HANDLE duplicated = NULL;
  HANDLE source = *(HANDLE const *)fds;
  BOOL ok = DuplicateHandle(GetCurrentProcess(), source, client_proc,
                            &duplicated, 0, FALSE, DUPLICATE_SAME_ACCESS);
  CloseHandle(client_proc);
  if (!ok)
    return -1;

  uintptr_t wire = (uintptr_t)duplicated;
  return texlink_socket_send(sock, &wire, sizeof(wire));
}

int texlink_recv_fds(texlink_socket_t sock, int *fds, int nfds) {
  if (!sock || !fds || nfds != 1)
    return -1;
  uintptr_t wire = 0;
  if (texlink_socket_recv(sock, &wire, sizeof(wire)) != 0)
    return -1;
  *(HANDLE *)fds = (HANDLE)wire;
  return 0;
}

int texlink_send_frame(texlink_socket_t sock, const texlink_frame_msg_t *msg,
                       int sync_fd) {
  (void)sync_fd;
  return texlink_socket_send(sock, msg, sizeof(*msg));
}

int texlink_recv_frame(texlink_socket_t sock, texlink_frame_msg_t *msg,
                       int *sync_fd) {
  if (sync_fd)
    *sync_fd = -1;
  return texlink_socket_recv(sock, msg, sizeof(*msg));
}
