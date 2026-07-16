#ifndef IPC_SYSTEM_STORAGE_SERVICE_H
#define IPC_SYSTEM_STORAGE_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int mounted;
  char mount_path[128];
  char fstype[64];
  uint64_t total_bytes;
  uint64_t free_bytes;
} StorageStatus;

int storage_init(const char *mount_path); /* default /mnt/sdcard */
void storage_deinit(void);

int storage_get_status(StorageStatus *st);
int storage_ensure_dirs(void); /* records/ snapshots/ */
int storage_cleanup_bad_files(void);

/* Free space ratio in [0,100]. Returns -1 on error. */
int storage_free_percent(void);

/* Delete oldest files under records/ until free_percent >= target_percent or nothing left.
 * Returns number of deleted files.
 */
int storage_recycle_oldest(int target_free_percent);

/* Delete up to `count` oldest mp4 files (for tests / forced cleanup). */
int storage_delete_oldest(int count);

const char *storage_mount_path(void);
const char *storage_records_path(void);

#ifdef __cplusplus
}
#endif

#endif
