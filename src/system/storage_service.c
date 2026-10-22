#include "storage_service.h"

#include "common/log.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

typedef struct {
  char path[512];
  time_t mtime;
  uint64_t size;
} FileEntry;

static char g_mount[128] = "/mnt/sdcard";
static char g_records[160] = "/mnt/sdcard/records";
static char g_snapshots[160] = "/mnt/sdcard/snapshots";
static char g_faces[160] = "/mnt/sdcard/faces";
static int g_ready = 0;
static int g_suspend_active;
static char g_sd_dev[64];
static char g_sd_driver_path[256];
static char g_sd_device_name[128];

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
  snprintf(g_snapshots, sizeof(g_snapshots), "%s/snapshots", g_mount);
  snprintf(g_faces, sizeof(g_faces), "%s/faces", g_mount);
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

const char *storage_snapshots_path(void)
{
  return g_snapshots;
}

const char *storage_faces_path(void)
{
  return g_faces;
}

static int sd_partition_present(void)
{
  return access("/dev/mmcblk1p1", F_OK) == 0 || access("/dev/mmcblk0p1", F_OK) == 0;
}

static const char *sd_partition_path(void)
{
  if (access("/dev/mmcblk1p1", F_OK) == 0) {
    return "/dev/mmcblk1p1";
  }
  if (access("/dev/mmcblk0p1", F_OK) == 0) {
    return "/dev/mmcblk0p1";
  }
  return "";
}

/* Superblock sniff so we can say 未格式化 instead of silently writing to a
 * raw partition (mkfs'd cards have no /proc/mounts entry until mounted). */
static int sd_partition_formatted(void)
{
  const char *dev = sd_partition_path();
  unsigned char buf[512];
  FILE *fp;
  size_t n;

  if (!dev[0]) {
    return 0;
  }
  fp = fopen(dev, "rb");
  if (!fp) {
    return 0;
  }
  n = fread(buf, 1, sizeof(buf), fp);
  fclose(fp);
  if (n < sizeof(buf)) {
    return 0;
  }
  if (buf[510] != 0x55 || buf[511] != 0xAA) {
    return 0;
  }
  /* FAT32/FAT16/FAT12 (offset 54) or FAT32 type at offset 82. */
  if (memcmp(buf + 54, "FAT", 3) == 0 || memcmp(buf + 82, "FAT32", 5) == 0) {
    return 1;
  }
  /* exFAT: "EXFAT   " at offset 3, 0x55AA at 510. */
  if (memcmp(buf + 3, "EXFAT   ", 8) == 0) {
    return 1;
  }
  return 0;
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

  st->present = sd_partition_present() ? 1 : 0;
  st->formatted = st->mounted ? 1 : (st->present && sd_partition_formatted());
  if (st->mounted) {
    snprintf(st->state, sizeof(st->state), "mounted");
  } else if (!st->present) {
    snprintf(st->state, sizeof(st->state), "nocard");
  } else if (st->formatted) {
    snprintf(st->state, sizeof(st->state), "unmounted");
  } else {
    snprintf(st->state, sizeof(st->state), "unformatted");
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

const char *storage_sd_device(void)
{
  return sd_partition_path();
}

int storage_ensure_dirs(void)
{
  char snap[160];
  char faces[160];
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
  snprintf(g_snapshots, sizeof(g_snapshots), "%s", snap);
  snprintf(faces, sizeof(faces), "%s/faces", g_mount);
  if (ensure_dir(faces) != 0) {
    return -1;
  }
  snprintf(g_faces, sizeof(g_faces), "%s", faces);
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
    (*list)[*count].size = (uint64_t)st.st_size;
    (*count)++;
  }
  closedir(dp);
  return 0;
}

static int collect_jpg(const char *dir, FileEntry **list, int *count, int *cap)
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
      collect_jpg(path, list, count, cap);
      continue;
    }
    n = strlen(de->d_name);
    if (n < 4 || strcmp(de->d_name + n - 4, ".jpg") != 0) {
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
    (*list)[*count].size = (uint64_t)st.st_size;
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

int storage_recycle_faces(int quota_percent)
{
  StorageStatus st;
  FileEntry *list = NULL;
  int count = 0, cap = 0, deleted = 0, i;
  uint64_t used = 0;
  uint64_t limit;

  if (!g_ready || quota_percent <= 0) {
    return -1;
  }
  if (quota_percent > 90) {
    quota_percent = 90;
  }
  if (storage_get_status(&st) != 0 || !st.mounted || st.total_bytes == 0) {
    return -1;
  }
  limit = (st.total_bytes * (uint64_t)quota_percent) / 100ULL;
  if (limit < (512ULL * 1024ULL)) {
    limit = 512ULL * 1024ULL;
  }
  if (collect_jpg(g_faces, &list, &count, &cap) != 0 || count == 0) {
    free(list);
    return 0;
  }
  for (i = 0; i < count; i++) {
    used += list[i].size;
  }
  if (used <= limit) {
    free(list);
    return 0;
  }
  qsort(list, (size_t)count, sizeof(FileEntry), cmp_mtime);
  for (i = 0; i < count && used > limit; i++) {
    if (unlink(list[i].path) == 0) {
      deleted++;
      used -= list[i].size;
      log_warn("storage", "face recycle delete: %s", list[i].path);
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
  log_info("storage", "face recycle removed %d files, quota=%d%%", deleted, quota_percent);
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

static int wipe_tree_files(const char *dir, int *deleted)
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
      wipe_tree_files(path, deleted);
      rmdir(path);
    } else if (unlink(path) == 0) {
      (*deleted)++;
    }
  }
  closedir(dp);
  return 0;
}

static int find_sd_bind_info(void)
{
  FILE *fp;
  char line[512];
  char dev[64], mnt[128], fstype[64];
  char syspath[256];
  char link[256];
  ssize_t n;

  g_sd_dev[0] = '\0';
  g_sd_driver_path[0] = '\0';
  g_sd_device_name[0] = '\0';

  fp = fopen("/proc/mounts", "r");
  if (!fp) {
    return -1;
  }
  while (fgets(line, sizeof(line), fp)) {
    if (sscanf(line, "%63s %127s %63s", dev, mnt, fstype) != 3) {
      continue;
    }
    if (strcmp(mnt, g_mount) != 0) {
      continue;
    }
    snprintf(g_sd_dev, sizeof(g_sd_dev), "%s", dev);
    break;
  }
  fclose(fp);
  if (!g_sd_dev[0]) {
    return -1;
  }

  /* /dev/mmcblk1p1 -> /sys/class/block/mmcblk1p1 -> ../../devices/.../mmc_host/... */
  {
    const char *base = strrchr(g_sd_dev, '/');
    base = base ? base + 1 : g_sd_dev;
    snprintf(syspath, sizeof(syspath), "/sys/class/block/%s", base);
  }
  n = readlink(syspath, link, sizeof(link) - 1);
  if (n <= 0) {
    log_warn("storage", "readlink %s failed", syspath);
    return 0; /* still allow umount-only */
  }
  link[n] = '\0';

  /* Walk up to find platform device bound to dwmmc / sdhci */
  {
    char path[512];
    char *p;
    snprintf(path, sizeof(path), "/sys/class/block/%s", strrchr(g_sd_dev, '/') + 1);
    /* Resolve real path via /sys/class/block/mmcblk1 (parent of partition) */
    {
      char parent[64];
      const char *bn = strrchr(g_sd_dev, '/') + 1;
      snprintf(parent, sizeof(parent), "%s", bn);
      /* strip partition suffix pN */
      p = parent;
      while (*p && !(*p == 'p' && p > parent && p[1] >= '0' && p[1] <= '9')) {
        p++;
      }
      if (*p == 'p') {
        *p = '\0';
      }
      snprintf(syspath, sizeof(syspath), "/sys/block/%s/device", parent);
      n = readlink(syspath, link, sizeof(link) - 1);
      if (n > 0) {
        link[n] = '\0';
        /* device -> ../../../ffa90000.mmc or similar */
        p = strrchr(link, '/');
        if (p) {
          snprintf(g_sd_device_name, sizeof(g_sd_device_name), "%s", p + 1);
        }
      }
    }
  }

  if (g_sd_device_name[0]) {
    /* Prefer dwmmc_rockchip driver unbind path */
    const char *drivers[] = {
        "/sys/bus/platform/drivers/dwmmc_rockchip",
        "/sys/bus/platform/drivers/rockchip-dw-mshc",
        NULL};
    int i;
    for (i = 0; drivers[i]; i++) {
      char bindpath[320];
      snprintf(bindpath, sizeof(bindpath), "%s/%s", drivers[i], g_sd_device_name);
      if (access(bindpath, F_OK) == 0) {
        snprintf(g_sd_driver_path, sizeof(g_sd_driver_path), "%s", drivers[i]);
        break;
      }
    }
  }
  return 0;
}

static int write_sysfs(const char *path, const char *val)
{
  int fd = open(path, O_WRONLY);
  if (fd < 0) {
    return -1;
  }
  if (write(fd, val, strlen(val)) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

int storage_prepare_suspend(void)
{
  char cmd[512];
  StorageStatus st;

  if (!g_ready) {
    return -1;
  }
  if (g_suspend_active) {
    return 0;
  }

  find_sd_bind_info();
  if (storage_get_status(&st) == 0 && st.mounted) {
    snprintf(cmd, sizeof(cmd), "umount %s", g_mount);
    if (system(cmd) != 0) {
      /* Lazy umount if busy (e.g. brief open fds); required for AOV mem path */
      snprintf(cmd, sizeof(cmd), "umount -l %s", g_mount);
      if (system(cmd) != 0) {
        log_error("storage", "umount %s failed", g_mount);
        return -1;
      }
      log_warn("storage", "umount -l %s (was busy)", g_mount);
    } else {
      log_info("storage", "umount %s ok", g_mount);
    }
  }

  if (g_sd_driver_path[0] && g_sd_device_name[0]) {
    char unbind[320];
    snprintf(unbind, sizeof(unbind), "%s/unbind", g_sd_driver_path);
    if (write_sysfs(unbind, g_sd_device_name) != 0) {
      log_warn("storage", "unbind %s failed (continue)", g_sd_device_name);
    } else {
      log_info("storage", "unbind %s from %s", g_sd_device_name, g_sd_driver_path);
    }
  }

  g_suspend_active = 1;
  return 0;
}

int storage_resume_from_suspend(void)
{
  char cmd[512];
  StorageStatus st;
  int i;

  if (!g_ready) {
    return -1;
  }
  if (!g_suspend_active) {
    /* Idempotent: ensure mounted */
    if (storage_get_status(&st) == 0 && st.mounted) {
      return 0;
    }
  }

  if (g_sd_driver_path[0] && g_sd_device_name[0]) {
    char bind[320];
    snprintf(bind, sizeof(bind), "%s/bind", g_sd_driver_path);
    write_sysfs(bind, g_sd_device_name);
    log_info("storage", "bind %s", g_sd_device_name);
  }

  for (i = 0; i < 30; i++) {
    if (g_sd_dev[0]) {
      snprintf(cmd, sizeof(cmd), "mount -t exfat %s %s 2>/dev/null || mount %s %s", g_sd_dev,
               g_mount, g_sd_dev, g_mount);
    } else {
      snprintf(cmd, sizeof(cmd), "mount /dev/mmcblk1p1 %s 2>/dev/null || mount /dev/mmcblk0p1 %s",
               g_mount, g_mount);
    }
    system(cmd);
    if (storage_get_status(&st) == 0 && st.mounted) {
      storage_ensure_dirs();
      g_suspend_active = 0;
      log_info("storage", "remount %s ok", g_mount);
      return 0;
    }
    usleep(100 * 1000);
  }

  log_error("storage", "remount %s failed", g_mount);
  return -1;
}

int storage_format_clear(void)
{
  char alarms[160];
  int deleted = 0;
  if (!g_ready) {
    return -1;
  }
  wipe_tree_files(g_records, &deleted);
  wipe_tree_files(g_snapshots, &deleted);
  wipe_tree_files(g_faces, &deleted);
  snprintf(alarms, sizeof(alarms), "%s/alarms", g_mount);
  wipe_tree_files(alarms, &deleted);
  if (storage_ensure_dirs() != 0) {
    return -2;
  }
  ensure_dir(alarms);
  log_warn("storage", "format_clear deleted %d files", deleted);
  return deleted;
}

int storage_format_card(void)
{
  char cmd[512];
  const char *dev;
  StorageStatus st;
  int i;
  int rc = 0;

  if (!g_ready) {
    return -1;
  }
  dev = sd_partition_path();
  if (!dev[0]) {
    log_error("storage", "format: no SD partition found");
    return -2;
  }

  /* Quiesce writers: clean umount first, lazy only if the card is still busy. */
  snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null", g_mount);
  if (system(cmd) != 0) {
    snprintf(cmd, sizeof(cmd), "umount -l %s 2>/dev/null", g_mount);
    if (system(cmd) != 0) {
      log_error("storage", "format: umount %s failed", g_mount);
      return -3;
    }
    log_warn("storage", "format: lazy umount %s (was busy)", g_mount);
  }

  /* vfat/FAT32 is the only filesystem the board mounts by default and
   * mkfs.vfat is present in the OEM rootfs. */
  snprintf(cmd, sizeof(cmd), "mkfs.vfat -F 32 -n IPC %s", dev);
  if (system(cmd) != 0) {
    log_error("storage", "format: mkfs.vfat %s failed", dev);
    rc = -4;
  } else {
    log_warn("storage", "format: mkfs.vfat %s ok", dev);
  }

  for (i = 0; i < 15; i++) {
    snprintf(cmd, sizeof(cmd), "mount -t vfat -o rw,umask=000 %s %s 2>/dev/null", dev, g_mount);
    if (system(cmd) != 0) {
      snprintf(cmd, sizeof(cmd), "mount %s %s 2>/dev/null", dev, g_mount);
      system(cmd);
    }
    if (storage_get_status(&st) == 0 && st.mounted) {
      g_suspend_active = 0;
      storage_ensure_dirs();
      ensure_dir(g_mount);
      log_info("storage", "format: remount %s ok", g_mount);
      return rc;
    }
    usleep(200 * 1000);
  }

  log_error("storage", "format: remount %s failed", g_mount);
  return rc != 0 ? rc : -5;
}
