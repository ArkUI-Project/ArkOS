#ifndef ARKFS2_H
#define ARKFS2_H
#include <stdbool.h>
#include <stdint.h>

/* Original ArkFS v2. Block numbers are 32-bit. The journal is 4 blocks.
 * Bitmap and inode blocks are copied to new blocks, then a commit sector is
 * written, then sector 0. A crash before sector 0 keeps the previous tree. */
typedef struct Arkfs2Disk {
    bool (*read)(uint32_t lba, uint32_t count, uint8_t *buf);
    bool (*write)(uint32_t lba, uint32_t count, const uint8_t *buf);
    bool (*flush)(void);
    uint32_t sectors;
} Arkfs2Disk;

bool arkfs2_format(Arkfs2Disk *disk);
bool arkfs2_mount(Arkfs2Disk *disk);
bool arkfs2_migrate_v1(Arkfs2Disk *disk);
void arkfs2_unmount(void);
const char *arkfs2_error(void);
uint64_t arkfs2_used_bytes(void);
uint64_t arkfs2_free_bytes(void);
bool arkfs2_card(char *out, uint32_t cap);
bool arkfs2_mkdir(const char *path);
bool arkfs2_write(const char *path, const void *data, uint64_t len);
bool arkfs2_read(const char *path, void *data, uint64_t cap, uint64_t *out_len);
bool arkfs2_rename(const char *from, const char *to);
bool arkfs2_remove(const char *path);
typedef bool (*Arkfs2Visit)(const char *path, int is_dir, uint64_t size, void *user);
bool arkfs2_visit(Arkfs2Visit visit, void *user);
bool arkfs2_set_readonly(const char *path, bool on);
bool arkfs2_meta(uint32_t *bitmap_block, uint32_t *inode_block, uint64_t *generation);
bool arkfs2_block(const char *path, uint32_t logical, uint32_t *block_id);
bool arkfs2_lookup(const char *path, uint32_t *ino, uint64_t *size, uint32_t *type);

#ifdef ARK_STORAGE_HOST_TEST
void arkfs2_fail_superblock(int times);
uint32_t arkfs2_inode_resident(void);
uint32_t arkfs2_inode_loads(void);
void arkfs2_reset_inode_loads(void);
#endif
#endif
