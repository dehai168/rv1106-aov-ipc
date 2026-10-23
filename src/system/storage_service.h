#ifndef IPC_SYSTEM_STORAGE_SERVICE_H
#define IPC_SYSTEM_STORAGE_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int mounted;
  int present;   /* SD block device exists */
  int formatted; /* recognised FAT/exFAT superblock on the partition */
  /* mounted | unmounted | unformatted | nocard */
  char state[24];
  char mount_path[128];
  char fstype[64];
  uint64_t total_bytes;
  uint64_t free_bytes;
} StorageStatus;

int storage_init(const char *mount_path); /* default /mnt/sdcard */
void storage_deinit(void);

int storage_get_status(StorageStatus *st);
int storage_ensure_dirs(void); /* records/ snapshots/ faces/ */
int storage_cleanup_bad_files(void);

/* Free space ratio in [0,100]. Returns -1 on error. */
int storage_free_percent(void);

/* Delete oldest files under records/ until free_percent >= target_percent or nothing left.
 * Returns number of deleted files.
 */
int storage_recycle_oldest(int target_free_percent);

/* snapshots/ name suffixes: YYYYMMDD_HHMMSS_md.jpg (motion link) and
 * YYYYMMDD_HHMMSS_mmm_face.jpg (face panorama). */
#define STORAGE_MOTION_SNAP_SUFFIX "_md.jpg"
#define STORAGE_FACE_SCENE_SUFFIX "_face.jpg"

/* Keep faces/ + face panoramas <= quota_percent of TF capacity; delete oldest. */
int storage_recycle_faces(int quota_percent);
/* Keep motion-link snapshots <= quota_percent of TF capacity; delete oldest. */
int storage_recycle_motion_snapshots(int quota_percent);

/* Delete up to `count` oldest mp4 files (for tests / forced cleanup). */
int storage_delete_oldest(int count);

/* Soft format: delete files under records/snapshots/alarms, recreate dirs. Not mkfs. */
int storage_format_clear(void);

/* Hard format: umount, mkfs.vfat -F 32 on the SD partition, remount, recreate
 * dirs. Returns 0 on success. Refuses while the card is busy. */
int storage_format_card(void);

const char *storage_sd_device(void); /* e.g. /dev/mmcblk1p1, "" when absent */

const char *storage_mount_path(void);
const char *storage_records_path(void);
const char *storage_snapshots_path(void);
const char *storage_faces_path(void);

/* AOV: umount TF + unbind SD host before mem; bind + mount after wake. */
int storage_prepare_suspend(void);
int storage_resume_from_suspend(void);

#ifdef __cplusplus
}
#endif

#endif
