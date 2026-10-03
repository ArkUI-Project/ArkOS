#ifndef ARK_NTFS_H
#define ARK_NTFS_H
#include "extfs.h"
/* Original read-only NTFS 3.x reader. One mounted volume; APIs are non-reentrant.
 * Callback LBAs use 512-byte sectors and are absolute on the supplied device.
 * Paths are absolute within the volume. Filenames are exact UTF-8 (case-sensitive).
 * A false emit return stops enumeration successfully. No write callback exists. */
bool ntfs_mount(FsReadBlocks read, void *ctx, uint64_t partition_lba, uint64_t partition_sectors);
bool ntfs_stat(const char *path, bool *is_dir, uint64_t *size);
bool ntfs_list(const char *path, FsEmit emit, void *ctx);
bool ntfs_read(const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *read);
const char *ntfs_error(void);
#endif
