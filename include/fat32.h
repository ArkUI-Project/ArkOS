#ifndef ARK_FAT32_H
#define ARK_FAT32_H
#include "extfs.h"
bool fat32_mount(FsReadBlocks read, FsWriteBlocks write, FsFlush flush, void *ctx, uint64_t start,
                 uint64_t sectors);
bool fat32_stat(const char *path, bool *is_dir, uint64_t *size);
bool fat32_list(const char *path, FsEmit emit, void *ctx);
bool fat32_read(const char *path, uint64_t offset, void *buffer, size_t bytes, size_t *read);
bool fat32_create(const char *path, bool directory);
bool fat32_write(const char *path, const void *buffer, size_t bytes);
bool fat32_remove(const char *path);
bool fat32_rename(const char *oldpath, const char *newpath);
bool fat32_sync(void);
bool fat32_writable(void);
const char *fat32_error(void);
#endif
