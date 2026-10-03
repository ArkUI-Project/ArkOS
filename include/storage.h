#ifndef ARK_STORAGE_H
#define ARK_STORAGE_H
#include "ark.h"
bool storage_init(void);
bool storage_mounted(void);
const char *storage_status(void);
uint64_t storage_capacity_bytes(void);
uint64_t storage_used_bytes(void);
bool storage_sync(void);
const char *storage_error(void);
bool vfs_mkdir(const char *path);
bool vfs_rename(const char *oldpath, const char *newpath);
bool vfs_copy(const char *oldpath, const char *newpath);
bool storage_disk_in_use(unsigned id);
bool storage_install_format(unsigned id, uint32_t start, uint32_t sectors);
bool storage_volume_io(uint32_t lba, uint32_t n, void *buf, bool write);
bool storage_volume_flush(void);
uint32_t storage_volume_sectors(void);
#endif
