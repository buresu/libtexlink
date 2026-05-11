#define _GNU_SOURCE
#include "texlink_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static int registry_open_locked(int flags) {
  mkdir("/tmp/texlink", 0755);

  int fd = open(TEXLINK_REGISTRY_PATH, flags | O_CLOEXEC, 0644);
  if (fd < 0)
    return -1;

  if (flock(fd, LOCK_EX) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void registry_read(int fd, texlink_registry_t *reg) {
  lseek(fd, 0, SEEK_SET);
  ssize_t n = read(fd, reg, sizeof(*reg));
  if (n < (ssize_t)sizeof(uint32_t) || reg->magic != TEXLINK_REGISTRY_MAGIC) {
    memset(reg, 0, sizeof(*reg));
    reg->magic = TEXLINK_REGISTRY_MAGIC;
  }
}

static int registry_write(int fd, const texlink_registry_t *reg) {
  lseek(fd, 0, SEEK_SET);
  if (ftruncate(fd, 0) < 0)
    return -1;
  ssize_t n = write(fd, reg, sizeof(*reg));
  return (n == (ssize_t)sizeof(*reg)) ? 0 : -1;
}

static int entry_alive(const texlink_registry_entry_t *e) {
  if (!e->active)
    return 0;
  return (kill(e->pid, 0) == 0 || errno == EPERM);
}

/*
 * Low-level: write name+path into the registry without a session object.
 * Used by texlink_serve_named() before accept() so consumers can connect.
 */
void texlink_registry_announce(const char *name, const char *path) {
  int fd = registry_open_locked(O_RDWR | O_CREAT);
  if (fd < 0)
    return;

  texlink_registry_t reg;
  registry_read(fd, &reg);

  /* Evict stale and same-name entries */
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg.entries[i];
    if (!e->active)
      continue;
    if (!entry_alive(e) || strncmp(e->name, name, TEXLINK_NAME_MAX) == 0)
      memset(e, 0, sizeof(*e));
  }

  int slot = -1;
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    if (!reg.entries[i].active) {
      slot = i;
      break;
    }
  }

  if (slot >= 0) {
    texlink_registry_entry_t *e = &reg.entries[slot];
    strncpy(e->name, name, TEXLINK_NAME_MAX - 1);
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->pid = getpid();
    e->active = 1;
    registry_write(fd, &reg);
  }

  flock(fd, LOCK_UN);
  close(fd);
}

int texlink_register(texlink_session_t *s, const char *name) {
  if (!s || !s->is_producer || !name || name[0] == '\0')
    return -1;

  strncpy(s->reg_name, name, TEXLINK_NAME_MAX - 1);

  int fd = registry_open_locked(O_RDWR | O_CREAT);
  if (fd < 0)
    return -1;

  texlink_registry_t reg;
  registry_read(fd, &reg);

  /* Evict stale entries and any previous entry with the same name */
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg.entries[i];
    if (!e->active)
      continue;
    if (!entry_alive(e) || strncmp(e->name, name, TEXLINK_NAME_MAX) == 0)
      memset(e, 0, sizeof(*e));
  }

  /* Find a free slot */
  int slot = -1;
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    if (!reg.entries[i].active) {
      slot = i;
      break;
    }
  }

  if (slot < 0) {
    flock(fd, LOCK_UN);
    close(fd);
    return -1; /* registry full */
  }

  texlink_registry_entry_t *e = &reg.entries[slot];
  strncpy(e->name, name, TEXLINK_NAME_MAX - 1);
  strncpy(e->path, s->sock_path, sizeof(e->path) - 1);
  e->pid = getpid();
  e->active = 1;

  registry_write(fd, &reg);
  flock(fd, LOCK_UN);
  close(fd);

  s->is_registered = 1;
  return 0;
}

void texlink_unregister(texlink_session_t *s) {
  if (!s || !s->is_registered)
    return;

  int fd = open(TEXLINK_REGISTRY_PATH, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    s->is_registered = 0;
    return;
  }

  if (flock(fd, LOCK_EX) < 0) {
    close(fd);
    s->is_registered = 0;
    return;
  }

  texlink_registry_t reg;
  registry_read(fd, &reg);

  pid_t my_pid = getpid();
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg.entries[i];
    if (e->active && e->pid == my_pid &&
        strncmp(e->name, s->reg_name, TEXLINK_NAME_MAX) == 0) {
      memset(e, 0, sizeof(*e));
      break;
    }
  }

  registry_write(fd, &reg);
  flock(fd, LOCK_UN);
  close(fd);

  s->is_registered = 0;
}

int texlink_list_sessions(char (*names)[TEXLINK_NAME_MAX], int max) {
  if (!names || max <= 0)
    return 0;

  int fd = open(TEXLINK_REGISTRY_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;

  if (flock(fd, LOCK_SH) < 0) {
    close(fd);
    return 0;
  }

  texlink_registry_t reg;
  registry_read(fd, &reg);
  flock(fd, LOCK_UN);
  close(fd);

  int count = 0;
  for (int i = 0; i < TEXLINK_MAX_SESSIONS && count < max; i++) {
    if (entry_alive(&reg.entries[i])) {
      strncpy(names[count], reg.entries[i].name, TEXLINK_NAME_MAX - 1);
      names[count][TEXLINK_NAME_MAX - 1] = '\0';
      count++;
    }
  }
  return count;
}

texlink_session_t *texlink_connect_by_name(const char *name) {
  if (!name)
    return NULL;

  int fd = open(TEXLINK_REGISTRY_PATH, O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return NULL;

  if (flock(fd, LOCK_SH) < 0) {
    close(fd);
    return NULL;
  }

  texlink_registry_t reg;
  registry_read(fd, &reg);
  flock(fd, LOCK_UN);
  close(fd);

  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg.entries[i];
    if (entry_alive(e) && strncmp(e->name, name, TEXLINK_NAME_MAX) == 0) {
      return texlink_connect(e->path);
    }
  }
  return NULL;
}
