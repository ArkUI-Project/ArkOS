#ifndef ARK_EXTFS_H
#define ARK_EXTFS_H
#include "ark.h"
typedef bool (*FsReadBlocks)(void *ctx, uint64_t lba, uint32_t sectors, void *buffer);
typedef bool (*FsWriteBlocks)(void *ctx, uint64_t lba, uint32_t sectors, const void *buffer);
typedef bool (*FsFlush)(void *ctx);
typedef bool (*FsEmit)(void *ctx, const char *name, bool is_dir, uint64_t size);
#define EXTFS_MAX_ENTRIES 64
typedef struct {
    bool mounted;
    bool read_only;
    uint64_t capacity_bytes;
    const char *mountpoint;
} ExtVolumeInfo;
bool extfs_volume_info(unsigned index, ExtVolumeInfo *info);
void extfs_init(void);
const char *extfs_status(void);
const char *extfs_error(void);
bool extfs_path(const char *path);
bool extfs_path_writable(const char *path);
int extfs_find(const char *path);
VFile *extfs_entry(int slot);
bool extfs_read(int slot);
bool extfs_read_bytes(int slot, uint64_t offset, void *buffer, size_t capacity, size_t *count);
bool extfs_list(const char *path);
int extfs_create(const char *path, bool directory);
bool extfs_write(int slot, const char *text);
bool extfs_remove(const char *path);
bool extfs_rename(const char *oldpath, const char *newpath);
bool extfs_sync(void);
/* Unified VFS facade. External indices begin at VFS_MAX_FILES. */
VFile *vfs_entry(int index);
int vfs_entry_limit(void);
bool vfs_read(int index);
bool vfs_list(const char *directory);
bool vfs_sync(void);
const char *vfs_error(void);
#endif
