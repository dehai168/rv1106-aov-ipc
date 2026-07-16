#include "storage_service.h"

#include "common/log.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct {
  char path[512];
  time_t mtime;
} FileEntry;

static char g_mount[128] = "/mnt/sdcard";
static char g_records[160] = "/mnt/sdcard/records";
static int g_ready = 0;

static int ensure_dir(const char *path)
{
  struct stat st;
  if (stat(path, &st) == 0) {
    return S_ISDIR(st.st_mode) ? 0 : -1;
  }
  if (mkdir(path, 0755) != 0 && errno != EEXIST) {
    log_error("storage", "mkdir %s failed: %s", path, strerror(errno));
    return -1;
  }
  return 0;
}

int storage_init(const char *mount_path)
{
  if (mount_path && mount_path[0]) {
    snprintf(g_mount, sizeof(g_mount), "%s", mount_path);
  }
  snprintf(g_records, sizeof(g_records), "%s/records", g_mount);
  g_ready = 1;
  return storage_ensure_dirs();
}

void storage_deinit(void)
{
  g_ready = 0;
}

const char *storage_mount_path(void)
{
  return g_mount;
}

const char *storage_records_path(void)
{
  return g_records;
}

int storage_get_status(StorageStatus *st)
{
  FILE *fp;
  char line[512];
  struct statvfs vfs;

  if (!st || !g_ready) {
    return -1;
  }
  memset(st, 0, sizeof(*st));
  snprintf(st->mount_path, sizeof(st->mount_path), "%s", g_mount);

  fp = fopen("/proc/mounts", "r");
  if (fp) {
    while (fgets(line, sizeof(line), fp)) {
      char dev[128], mnt[128], fstype[64];
      if (sscanf(line, "%127s %127s %63s", dev, mnt, fstype) == 3) {
        if (strcmp(mnt, g_mount) == 0) {
          st->mounted = 1;
          snprintf(st->fstype, sizeof(st->fstype), "%s", fstype);
          break;
        }
      }
    }
    fclose(fp);
  }

  if (!st->mounted) {
    return 0;
  }
  if (statvfs(g_mount, &vfs) != 0) {
    log_error("storage", "statvfs %s failed: %s", g_mount, strerror(errno));
    return -1;
  }
  st->total_bytes = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
  st->free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
  return 0;
}

int storage_ensure_dirs(void)
{
  char snap[160];
  if (ensure_dir(g_mount) != 0) {
    return -1;
  }
  if (ensure_dir(g_records) != 0) {
    return -1;
  }
  snprintf(snap, sizeof(snap), "%s/snapshots", g_mount);
  if (ensure_dir(snap) != 0) {
    return -1;
  }
  return 0;
}

int storage_free_percent(void)
{
  StorageStatus st;
  if (storage_get_status(&st) != 0 || !st.mounted || st.total_bytes == 0) {
    return -1;
  }
  return (int)((st.free_bytes * 100ULL) / st.total_bytes);
}

static int is_bad_name(const char *name)
{
  size_t n = strlen(name);
  if (n >= 4 && strcmp(name + n - 4, ".tmp") == 0) {
    return 1;
  }
  if (n >= 5 && strcmp(name + n - 5, ".part") == 0) {
    return 1;
  }
  return 0;
}

static int walk_cleanup(const char *dir, int *deleted)
{
  DIR *dp = opendir(dir);
  struct dirent *de;
  if (!dp) {
    return -1;
  }
  while ((de = readdir(dp)) != NULL) {
    char path[512];
    struct stat st;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      walk_cleanup(path, deleted);
    } else if (is_bad_name(de->d_name) || st.st_size == 0) {
      if (unlink(path) == 0) {
        (*deleted)++;
        log_warn("storage", "removed bad file: %s", path);
      }
    }
  }
  closedir(dp);
  return 0;
}

int storage_cleanup_bad_files(void)
{
  int deleted = 0;
  if (!g_ready) {
    return -1;
  }
  walk_cleanup(g_records, &deleted);
  log_info("storage", "cleanup deleted %d bad files", deleted);
  return deleted;
}

static int collect_mp4(const char *dir, FileEntry **list, int *count, int *cap)
{
  DIR *dp = opendir(dir);
  struct dirent *de;
  if (!dp) {
    return -1;
  }
  while ((de = readdir(dp)) != NULL) {
    char path[512];
    struct stat st;
    size_t n;
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
      continue;
    }
    snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
    if (stat(path, &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      collect_mp4(path, list, count, cap);
      continue;
    }
    n = strlen(de->d_name);
    if (n < 4 || strcmp(de->d_name + n - 4, ".mp4") != 0) {
      continue;
    }
    if (*count >= *cap) {
      int ncap = (*cap == 0) ? 64 : (*cap * 2);
      FileEntry *nl = (FileEntry *)realloc(*list, (size_t)ncap * sizeof(FileEntry));
      if (!nl) {
        closedir(dp);
        return -1;
      }
      *list = nl;
      *cap = ncap;
    }
    snprintf((*list)[*count].path, sizeof((*list)[*count].path), "%s", path);
    (*list)[*count].mtime = st.st_mtime;
    (*count)++;
  }
  closedir(dp);
  return 0;
}

static int cmp_mtime(const void *a, const void *b)
{
  const FileEntry *fa = (const FileEntry *)a;
  const FileEntry *fb = (const FileEntry *)b;
  if (fa->mtime < fb->mtime) {
    return -1;
  }
  if (fa->mtime > fb->mtime) {
    return 1;
  }
  return 0;
}

int storage_recycle_oldest(int target_free_percent)
{
  FileEntry *list = NULL;
  int count = 0, cap = 0, deleted = 0;

  if (!g_ready || target_free_percent <= 0) {
    return -1;
  }

  while (storage_free_percent() >= 0 && storage_free_percent() < target_free_percent) {
    count = 0;
    free(list);
    list = NULL;
    cap = 0;
    if (collect_mp4(g_records, &list, &count, &cap) != 0 || count == 0) {
      break;
    }
    qsort(list, (size_t)count, sizeof(FileEntry), cmp_mtime);
    if (unlink(list[0].path) == 0) {
      deleted++;
      log_warn("storage", "recycle delete: %s", list[0].path);
    } else {
      break;
    }
    /* also try remove empty day dir */
    {
      char *slash = strrchr(list[0].path, '/');
      if (slash) {
        *slash = '\0';
        rmdir(list[0].path);
      }
    }
  }

  free(list);
  log_info("storage", "recycle removed %d files, free=%d%%", deleted, storage_free_percent());
  return deleted;
}

int storage_delete_oldest(int count)
{
  FileEntry *list = NULL;
  int n = 0, cap = 0, deleted = 0, i;

  if (!g_ready || count <= 0) {
    return -1;
  }
  if (collect_mp4(g_records, &list, &n, &cap) != 0 || n == 0) {
    free(list);
    return 0;
  }
  qsort(list, (size_t)n, sizeof(FileEntry), cmp_mtime);
  for (i = 0; i < count && i < n; i++) {
    if (unlink(list[i].path) == 0) {
      deleted++;
      log_warn("storage", "delete oldest: %s", list[i].path);
      {
        char *slash = strrchr(list[i].path, '/');
        if (slash) {
          *slash = '\0';
          rmdir(list[i].path);
        }
      }
    }
  }
  free(list);
  return deleted;
}
