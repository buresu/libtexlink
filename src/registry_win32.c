#include "texlink_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HANDLE registry_mutex(void) {
  return CreateMutexA(NULL, FALSE, "Local\\texlink_registry_mutex");
}

static HANDLE registry_mapping(DWORD protect, DWORD access, int create) {
  (void)access;
  char temp[MAX_PATH];
  DWORD len = GetTempPathA(sizeof(temp), temp);
  if (len == 0 || len >= sizeof(temp))
    return NULL;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%stexlink.registry", temp);

  DWORD creation = create ? OPEN_ALWAYS : OPEN_EXISTING;
  HANDLE file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, creation,
                            FILE_ATTRIBUTE_NORMAL, NULL);
  if (file == INVALID_HANDLE_VALUE)
    return NULL;

  LARGE_INTEGER size;
  size.QuadPart = sizeof(texlink_registry_t);
  SetFilePointerEx(file, size, NULL, FILE_BEGIN);
  SetEndOfFile(file);

  HANDLE mapping =
      CreateFileMappingA(file, NULL, protect, 0, sizeof(texlink_registry_t),
                         TEXLINK_REGISTRY_PATH);
  CloseHandle(file);
  return mapping;
}

static texlink_registry_t *registry_map(HANDLE mapping, DWORD access) {
  return MapViewOfFile(mapping, access, 0, 0, sizeof(texlink_registry_t));
}

static void registry_init(texlink_registry_t *reg) {
  if (reg->magic != TEXLINK_REGISTRY_MAGIC) {
    memset(reg, 0, sizeof(*reg));
    reg->magic = TEXLINK_REGISTRY_MAGIC;
  }
}

static int entry_alive(const texlink_registry_entry_t *e) {
  if (!e->active)
    return 0;
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, e->pid);
  if (!process)
    return 0;
  DWORD code = 0;
  BOOL alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive ? 1 : 0;
}

void texlink_registry_announce(const char *name, const char *path) {
  if (!name || !path)
    return;

  HANDLE mutex = registry_mutex();
  if (!mutex)
    return;
  WaitForSingleObject(mutex, INFINITE);

  HANDLE mapping = registry_mapping(PAGE_READWRITE, FILE_MAP_ALL_ACCESS, 1);
  if (!mapping)
    goto out_mutex;

  texlink_registry_t *reg = registry_map(mapping, FILE_MAP_ALL_ACCESS);
  if (!reg)
    goto out_mapping;
  registry_init(reg);

  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg->entries[i];
    if (!e->active)
      continue;
    if (!entry_alive(e) || strncmp(e->name, name, TEXLINK_NAME_MAX) == 0)
      memset(e, 0, sizeof(*e));
  }

  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg->entries[i];
    if (e->active)
      continue;
    strncpy(e->name, name, TEXLINK_NAME_MAX - 1);
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->pid = GetCurrentProcessId();
    e->active = 1;
    break;
  }

  UnmapViewOfFile(reg);
out_mapping:
  CloseHandle(mapping);
out_mutex:
  ReleaseMutex(mutex);
  CloseHandle(mutex);
}

int texlink_registry_unregister(const char *name) {
  if (!name || name[0] == '\0')
    return -1;

  HANDLE mutex = registry_mutex();
  if (!mutex)
    return -1;
  WaitForSingleObject(mutex, INFINITE);

  HANDLE mapping = registry_mapping(PAGE_READWRITE, FILE_MAP_ALL_ACCESS, 0);
  if (!mapping)
    goto out_mutex;

  texlink_registry_t *reg = registry_map(mapping, FILE_MAP_ALL_ACCESS);
  if (!reg)
    goto out_mapping;
  registry_init(reg);

  DWORD pid = GetCurrentProcessId();
  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg->entries[i];
    if (e->active && e->pid == pid &&
        strncmp(e->name, name, TEXLINK_NAME_MAX) == 0) {
      memset(e, 0, sizeof(*e));
      break;
    }
  }

  UnmapViewOfFile(reg);
out_mapping:
  CloseHandle(mapping);
out_mutex:
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return 0;
}

int texlink_registry_list(char (*names)[TEXLINK_NAME_MAX], int max) {
  if (!names || max <= 0)
    return 0;

  HANDLE mutex = registry_mutex();
  if (!mutex)
    return 0;
  WaitForSingleObject(mutex, INFINITE);

  int count = 0;
  HANDLE mapping = registry_mapping(PAGE_READONLY, FILE_MAP_READ, 0);
  if (!mapping)
    goto out_mutex;

  texlink_registry_t *reg = registry_map(mapping, FILE_MAP_READ);
  if (!reg)
    goto out_mapping;
  registry_init(reg);

  for (int i = 0; i < TEXLINK_MAX_SESSIONS && count < max; i++) {
    if (entry_alive(&reg->entries[i])) {
      strncpy(names[count], reg->entries[i].name, TEXLINK_NAME_MAX - 1);
      names[count][TEXLINK_NAME_MAX - 1] = '\0';
      count++;
    }
  }

  UnmapViewOfFile(reg);
out_mapping:
  CloseHandle(mapping);
out_mutex:
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return count;
}

int texlink_registry_resolve(const char *name, char *path, size_t path_size) {
  if (!name || !path || path_size == 0)
    return -1;

  HANDLE mutex = registry_mutex();
  if (!mutex)
    return -1;
  WaitForSingleObject(mutex, INFINITE);

  int ret = -1;
  HANDLE mapping = registry_mapping(PAGE_READONLY, FILE_MAP_READ, 0);
  if (!mapping)
    goto out_mutex;

  texlink_registry_t *reg = registry_map(mapping, FILE_MAP_READ);
  if (!reg)
    goto out_mapping;
  registry_init(reg);

  for (int i = 0; i < TEXLINK_MAX_SESSIONS; i++) {
    texlink_registry_entry_t *e = &reg->entries[i];
    if (entry_alive(e) && strncmp(e->name, name, TEXLINK_NAME_MAX) == 0) {
      strncpy(path, e->path, path_size - 1);
      path[path_size - 1] = '\0';
      ret = 0;
      break;
    }
  }

  UnmapViewOfFile(reg);
out_mapping:
  CloseHandle(mapping);
out_mutex:
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return ret;
}
