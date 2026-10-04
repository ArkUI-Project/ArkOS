/* Original ArkFS v2. Not derived from ext2, FAT, or littlefs.
 * 4KB blocks. Eight direct block numbers, then a chain of indirect blocks
 * (1023 data slots and one next-block slot). Directory entries are
 * inode + name-length + name and never cross a block. */
#include "arkfs2.h"
#include <stddef.h>

#define SECTOR 512u
#define BLOCK 4096u
#define JOURNAL_BLOCKS 4u
#define DIRECT 8u
#define INDIRECT_SLOTS 1023u
#define INODE_BYTES 64u
#define INODES_PER_BLOCK 64u
#define MAX_BITMAP 4u
#define INDEX_DATA 1023u
#define ICACHE 8u
#define MAX_CHAIN 128u
#define NAME_MAX 255u
#define PATH_MAX 1023u
#define V1_BANK_SECTORS 2080u
#define V1_BANK0 8u
#define V1_BANK1 (V1_BANK0 + V1_BANK_SECTORS)
#define V1_PREFIX_SECTORS (V1_BANK1 + V1_BANK_SECTORS)
#define V1_RECORD 136u

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t size;
    uint64_t mtime;
    uint32_t direct[8];
    uint32_t indirect;
} Inode;

static Arkfs2Disk *disk;
static const char *last_error = "";
static bool mounted, building;
static uint32_t total_blocks, journal_block, bitmap_count, index_block, iblock_count, reserved_prefix;
static uint32_t bitmap_loc[MAX_BITMAP];
static uint64_t generation;
static uint64_t used_live;
static uint8_t bitmap_mem[MAX_BITMAP * BLOCK];
static uint8_t committed_mem[MAX_BITMAP * BLOCK];
static uint8_t quarantine_mem[MAX_BITMAP * BLOCK];
typedef struct {
    uint32_t index;
    uint32_t loc;
    uint8_t used, dirty, fresh;
    Inode ent[INODES_PER_BLOCK];
} IBlock;
static IBlock icache[ICACHE];
static uint32_t words_cache_block[2];
static uint32_t words_cache[2][1024];
static uint8_t words_cache_hand;
static uint32_t inode_loads;
_Static_assert(sizeof(icache) < 64u * 1024u, "inode cache must not be a resident table");
#ifdef ARK_STORAGE_HOST_TEST
static int fail_superblock;
#endif

static uint32_t crc32(const uint8_t *bytes, size_t length) {
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < length; ++i) {
        c ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
    }
    return c ^ 0xffffffffu;
}
static uint32_t get32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t get64(const uint8_t *b) {
    return get32(b) | ((uint64_t)get32(b + 4) << 32);
}
static void put32(uint8_t *b, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i)
        b[i] = (uint8_t)(n >> (i * 8));
}
static void put64(uint8_t *b, uint64_t n) {
    put32(b, (uint32_t)n);
    put32(b + 4, (uint32_t)(n >> 32));
}
static void mem_set(void *dst, int value, size_t n) {
    uint8_t *p = dst;
    for (size_t i = 0; i < n; ++i)
        p[i] = (uint8_t)value;
}
static void mem_copy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst;
    const uint8_t *s = src;
    for (size_t i = 0; i < n; ++i)
        d[i] = s[i];
}
static bool mem_eq(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; ++i)
        if (x[i] != y[i])
            return false;
    return true;
}
static bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
static bool bit_get(uint32_t block) {
    return (bitmap_mem[block >> 3] >> (block & 7)) & 1u;
}
static bool committed_get(uint32_t block) {
    return (committed_mem[block >> 3] >> (block & 7)) & 1u;
}
static bool quarantine_get(uint32_t block) {
    return (quarantine_mem[block >> 3] >> (block & 7)) & 1u;
}
static void snapshot_bits(void) {
    for (size_t i = 0; i < sizeof quarantine_mem; ++i)
        quarantine_mem[i] = (uint8_t)(committed_mem[i] & (uint8_t)~bitmap_mem[i]);
    mem_copy(committed_mem, bitmap_mem, sizeof committed_mem);
}
static void bit_set(uint32_t block, bool used) {
    uint8_t mask = (uint8_t)(1u << (block & 7));
    if (used)
        bitmap_mem[block >> 3] |= mask;
    else
        bitmap_mem[block >> 3] &= (uint8_t)~mask;
}
static bool read_block(uint32_t block, uint8_t *buf) {
    return block && block < total_blocks && disk->read(block * 8u, 8, buf);
}
static bool write_block(uint32_t block, const uint8_t *buf) {
    if (!block || block >= total_blocks)
        return false;
    if (reserved_prefix && block < reserved_prefix)
        return false;
    for (unsigned s = 0; s < 2; ++s)
        if (words_cache_block[s] == block)
            words_cache_block[s] = 0;
    return disk->write(block * 8u, 8, buf);
}
static uint32_t alloc_block(void) {
    for (uint32_t b = 1; b < total_blocks; ++b) {
        if (!bit_get(b) && !committed_get(b) && !quarantine_get(b)) {
            bit_set(b, true);
            return b;
        }
    }
    last_error = "空间不足，未保存";
    return 0;
}
static void free_block(uint32_t block) {
    if (block && block < total_blocks && !(reserved_prefix && block < reserved_prefix))
        bit_set(block, false);
}
static void pack_inode(const Inode *in, uint8_t *p) {
    mem_set(p, 0, INODE_BYTES);
    put32(p, in->type);
    put32(p + 4, in->flags);
    put64(p + 8, in->size);
    put64(p + 16, in->mtime);
    for (unsigned i = 0; i < 8; ++i)
        put32(p + 24 + i * 4, in->direct[i]);
    put32(p + 56, in->indirect);
}
static void unpack_inode(const uint8_t *p, Inode *in) {
    mem_set(in, 0, sizeof *in);
    in->type = get32(p);
    in->flags = get32(p + 4);
    in->size = get64(p + 8);
    in->mtime = get64(p + 16);
    for (unsigned i = 0; i < 8; ++i)
        in->direct[i] = get32(p + 24 + i * 4);
    in->indirect = get32(p + 56);
}
static void load_words(uint32_t block, uint32_t *words) {
    mem_set(words, 0, 1024u * sizeof(uint32_t));
    if (!block)
        return;
    for (unsigned slot = 0; slot < 2; ++slot) {
        if (words_cache_block[slot] == block) {
            mem_copy(words, words_cache[slot], sizeof words_cache[slot]);
            return;
        }
    }
    uint8_t raw[BLOCK];
    if (!read_block(block, raw))
        return;
    for (unsigned i = 0; i < 1024; ++i)
        words[i] = get32(raw + i * 4);
    unsigned slot = words_cache_hand++ & 1u;
    words_cache_block[slot] = block;
    mem_copy(words_cache[slot], words, sizeof words_cache[slot]);
}
static bool words_empty(const uint32_t *words) {
    for (unsigned i = 0; i < 1024; ++i)
        if (words[i])
            return false;
    return true;
}
static bool cow_words(uint32_t *block, const uint32_t *words) {
    uint32_t neu = alloc_block();
    if (!neu)
        return false;
    uint8_t raw[BLOCK];
    for (unsigned i = 0; i < 1024; ++i)
        put32(raw + i * 4, words[i]);
    if (!write_block(neu, raw))
        return false;
    if (*block)
        free_block(*block);
    *block = neu;
    return true;
}
static uint32_t map_get(const Inode *ino, uint32_t logical) {
    if (logical < DIRECT)
        return ino->direct[logical];
    uint32_t rest = logical - DIRECT, link = ino->indirect;
    while (rest >= INDIRECT_SLOTS) {
        if (!link)
            return 0;
        uint32_t words[1024];
        load_words(link, words);
        link = words[1023];
        rest -= INDIRECT_SLOTS;
    }
    if (!link)
        return 0;
    uint32_t words[1024];
    load_words(link, words);
    return words[rest];
}
static bool map_set(Inode *ino, uint32_t logical, uint32_t value, uint32_t *old) {
    *old = 0;
    if (logical < DIRECT) {
        *old = ino->direct[logical];
        ino->direct[logical] = value;
        return true;
    }
    uint32_t rest = logical - DIRECT;
    uint32_t hops = rest / INDIRECT_SLOTS;
    uint32_t slot = rest % INDIRECT_SLOTS;
    if (hops >= MAX_CHAIN)
        return false;
    uint32_t ids[MAX_CHAIN];
    mem_set(ids, 0, sizeof ids);
    uint32_t link = ino->indirect;
    for (uint32_t h = 0; h <= hops && link; ++h) {
        ids[h] = link;
        if (h == hops)
            break;
        uint32_t words[1024];
        load_words(link, words);
        link = words[1023];
    }
    if (!value && !ids[hops])
        return true;
    uint32_t words[1024];
    load_words(ids[hops], words);
    *old = words[slot];
    if (*old == value && ids[hops])
        return true;
    words[slot] = value;
    uint32_t leaf = ids[hops];
    if (words_empty(words)) {
        if (leaf)
            free_block(leaf);
        leaf = 0;
    } else if (!cow_words(&leaf, words))
        return false;
    for (int h = (int)hops - 1; h >= 0; --h) {
        load_words(ids[h], words);
        words[1023] = leaf;
        uint32_t blk = ids[h];
        if (words_empty(words)) {
            if (blk)
                free_block(blk);
            leaf = 0;
        } else {
            if (!cow_words(&blk, words))
                return false;
            leaf = blk;
        }
    }
    ino->indirect = leaf;
    return true;
}
static void icache_clear(void) {
    mem_set(icache, 0, sizeof icache);
    mem_set(words_cache_block, 0, sizeof words_cache_block);
}
static IBlock *icache_find(uint32_t index) {
    for (uint32_t i = 0; i < ICACHE; ++i)
        if (icache[i].used && icache[i].index == index)
            return &icache[i];
    return 0;
}
static IBlock *icache_slot(void) {
    for (uint32_t i = 0; i < ICACHE; ++i)
        if (!icache[i].used)
            return &icache[i];
    for (uint32_t i = 0; i < ICACHE; ++i)
        if (icache[i].used && !icache[i].dirty)
            return &icache[i];
    return 0;
}
static bool index_lookup(uint32_t iblock, uint32_t *loc) {
    IBlock *hit = icache_find(iblock);
    if (hit && hit->fresh) {
        *loc = hit->loc;
        return hit->loc != 0;
    }
    uint32_t hop = iblock / INDEX_DATA;
    uint32_t slot = iblock % INDEX_DATA;
    uint32_t link = index_block;
    for (uint32_t h = 0;; ++h) {
        if (!link)
            return false;
        uint32_t words[1024];
        load_words(link, words);
        if (h == hop) {
            *loc = words[slot];
            return *loc != 0;
        }
        link = words[1023];
    }
}
static bool load_iblock(IBlock *c, uint32_t index) {
    uint32_t loc = 0;
    if (!index_lookup(index, &loc))
        return false;
    uint8_t raw[BLOCK];
    if (!read_block(loc, raw))
        return false;
    ++inode_loads;
    mem_set(c, 0, sizeof *c);
    c->used = 1;
    c->index = index;
    c->loc = loc;
    for (uint32_t s = 0; s < INODES_PER_BLOCK; ++s)
        unpack_inode(raw + s * INODE_BYTES, &c->ent[s]);
    return true;
}
static IBlock *hold_iblock(uint32_t index) {
    IBlock *c = icache_find(index);
    if (c)
        return c;
    c = icache_slot();
    if (!c || !load_iblock(c, index))
        return 0;
    return c;
}
static Inode *ino_ref(uint32_t ino) {
    IBlock *c = hold_iblock(ino / INODES_PER_BLOCK);
    if (!c) {
        static Inode none;
        mem_set(&none, 0, sizeof none);
        return &none;
    }
    return &c->ent[ino % INODES_PER_BLOCK];
}
static void mark_inode(uint32_t ino) {
    IBlock *c = icache_find(ino / INODES_PER_BLOCK);
    if (c)
        c->dirty = 1;
}
static uint32_t alloc_inode(void) {
    if (iblock_count) {
        IBlock *last = hold_iblock(iblock_count - 1);
        if (last) {
            uint32_t base = (iblock_count - 1) * INODES_PER_BLOCK;
            uint32_t s0 = base ? 0u : 1u;
            for (uint32_t s = s0; s < INODES_PER_BLOCK; ++s)
                if (!last->ent[s].type)
                    return base + s;
        }
    }
    uint32_t block = alloc_block();
    if (!block)
        return 0;
    IBlock *c = icache_slot();
    if (!c) {
        free_block(block);
        return 0;
    }
    mem_set(c, 0, sizeof *c);
    c->used = 1;
    c->dirty = 1;
    c->fresh = 1;
    c->index = iblock_count;
    c->loc = block;
    uint32_t base = iblock_count * INODES_PER_BLOCK;
    ++iblock_count;
    return base ? base : 1;
}
static bool utf8_ok(const char *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (c < 0x20 || c == 0x7f)
            return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        uint32_t need = c < 0xe0 ? 1u : c < 0xf0 ? 2u : c < 0xf8 ? 3u : 4u;
        if (c < 0xc2 || c > 0xf4 || need > 3 || i + need >= n)
            return false;
        for (uint32_t k = 1; k <= need; ++k)
            if (((uint8_t)s[i + k] & 0xc0) != 0x80)
                return false;
        uint8_t c1 = (uint8_t)s[i + 1];
        if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 >= 0xa0) ||
            (c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 >= 0x90))
            return false;
        i += need + 1;
    }
    return true;
}
static bool canonicalize(const char *in, char *out) {
    if (!in || !in[0]) {
        last_error = "名称不能为空";
        return false;
    }
    uint32_t pos = 1, n = 0, endpos[512];
    out[0] = '/';
    const char *p = in;
    if (*p == '/')
        ++p;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            ++p;
        uint32_t len = (uint32_t)(p - start);
        if (len == 1 && start[0] == '.') {
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (n) {
                --n;
                pos = n ? endpos[n - 1] : 1;
            }
        } else if (len) {
            if (len > NAME_MAX) {
                last_error = "文件名过长";
                return false;
            }
            if (!utf8_ok(start, len)) {
                last_error = "名称无效";
                return false;
            }
            uint32_t next = pos + (n ? 1u : 0u) + len;
            if (next > PATH_MAX || n >= 512) {
                last_error = "路径过长";
                return false;
            }
            if (n)
                out[pos++] = '/';
            mem_copy(out + pos, start, len);
            pos += len;
            endpos[n++] = pos;
        }
        if (*p == '/')
            ++p;
    }
    out[pos] = 0;
    return true;
}
static bool split_last(const char *canon, char *parent, char *name, uint32_t *nlen) {
    if (str_eq(canon, "/")) {
        last_error = "名称不能为空";
        return false;
    }
    uint32_t last = 0, i = 0;
    while (canon[i]) {
        if (canon[i] == '/')
            last = i;
        ++i;
    }
    uint32_t len = i - last - 1;
    if (!len || len > NAME_MAX) {
        last_error = "名称不能为空";
        return false;
    }
    mem_copy(name, canon + last + 1, len);
    name[len] = 0;
    *nlen = len;
    if (!last) {
        parent[0] = '/';
        parent[1] = 0;
    } else {
        mem_copy(parent, canon, last);
        parent[last] = 0;
    }
    return true;
}
static bool dir_walk(const Inode *dir, const char *name, uint32_t nlen, uint32_t *ino, uint32_t *block_index, uint32_t *offset) {
    uint32_t blocks = (uint32_t)(dir->size / BLOCK);
    for (uint32_t b = 0; b < blocks; ++b) {
        uint32_t id = map_get(dir, b);
        uint8_t raw[BLOCK];
        if (!id || !read_block(id, raw))
            return false;
        uint32_t off = 0;
        while (off + 5 <= BLOCK) {
            uint32_t eino = get32(raw + off);
            uint8_t elen = raw[off + 4];
            if (!eino && !elen)
                break;
            if (!elen || off + 5u + elen > BLOCK)
                return false;
            if (elen == nlen && mem_eq(raw + off + 5, name, nlen)) {
                if (ino)
                    *ino = eino;
                if (block_index)
                    *block_index = b;
                if (offset)
                    *offset = off;
                return true;
            }
            off += 5u + elen;
        }
    }
    return false;
}
static bool dir_insert(Inode *dir, uint32_t ino, const char *name, uint32_t nlen) {
    uint32_t need = 5u + nlen;
    uint32_t blocks = (uint32_t)(dir->size / BLOCK);
    for (uint32_t b = 0; b < blocks; ++b) {
        uint32_t id = map_get(dir, b);
        uint8_t raw[BLOCK];
        if (!id || !read_block(id, raw))
            return false;
        uint32_t off = 0;
        while (off + 5 <= BLOCK) {
            uint32_t eino = get32(raw + off);
            uint8_t elen = raw[off + 4];
            if (!eino && !elen)
                break;
            if (!elen || off + 5u + elen > BLOCK)
                return false;
            off += 5u + elen;
        }
        if (off + need <= BLOCK) {
            put32(raw + off, ino);
            raw[off + 4] = (uint8_t)nlen;
            mem_copy(raw + off + 5, name, nlen);
            off += need;
            if (off + 5 <= BLOCK) {
                put32(raw + off, 0);
                raw[off + 4] = 0;
            }
            uint32_t neu = alloc_block();
            uint32_t old = 0;
            if (!neu || !write_block(neu, raw) || !map_set(dir, b, neu, &old))
                return false;
            if (old)
                free_block(old);
            return true;
        }
    }
    uint8_t raw[BLOCK];
    mem_set(raw, 0, sizeof raw);
    put32(raw, ino);
    raw[4] = (uint8_t)nlen;
    mem_copy(raw + 5, name, nlen);
    if (need + 5 <= BLOCK)
        raw[need + 4] = 0;
    uint32_t neu = alloc_block();
    uint32_t old = 0;
    if (!neu || !write_block(neu, raw) || !map_set(dir, blocks, neu, &old))
        return false;
    if (old)
        free_block(old);
    dir->size = (uint64_t)(blocks + 1) * BLOCK;
    return true;
}
static bool dir_remove(Inode *dir, const char *name, uint32_t nlen) {
    uint32_t b = 0, off = 0;
    if (!dir_walk(dir, name, nlen, 0, &b, &off))
        return false;
    uint32_t id = map_get(dir, b);
    uint8_t raw[BLOCK];
    if (!id || !read_block(id, raw))
        return false;
    uint8_t elen = raw[off + 4];
    uint32_t next = off + 5u + elen;
    for (uint32_t i = off; i + (next - off) < BLOCK; ++i)
        raw[i] = raw[i + (next - off)];
    for (uint32_t i = BLOCK - (next - off); i < BLOCK; ++i)
        raw[i] = 0;
    uint32_t neu = alloc_block();
    uint32_t old = 0;
    if (!neu || !write_block(neu, raw) || !map_set(dir, b, neu, &old))
        return false;
    if (old)
        free_block(old);
    return true;
}
static bool lookup_path(const char *canon, uint32_t *ino) {
    if (str_eq(canon, "/")) {
        *ino = 1;
        return ino_ref(1)->type == 2;
    }
    char parent[1024], name[256];
    uint32_t nlen = 0, pin = 0;
    if (!split_last(canon, parent, name, &nlen) || !lookup_path(parent, &pin))
        return false;
    if (ino_ref(pin)->type != 2)
        return false;
    return dir_walk(ino_ref(pin), name, nlen, ino, 0, 0);
}
static bool write_bytes(Inode *ino, const uint8_t *data, uint64_t len) {
    uint32_t new_blocks = len ? (uint32_t)((len + BLOCK - 1) / BLOCK) : 0;
    uint32_t old_blocks = ino->size ? (uint32_t)((ino->size + BLOCK - 1) / BLOCK) : 0;
    for (uint32_t i = 0; i < new_blocks; ++i) {
        uint8_t raw[BLOCK];
        mem_set(raw, 0, sizeof raw);
        uint64_t off = (uint64_t)i * BLOCK;
        uint32_t n = (uint32_t)((len - off) > BLOCK ? BLOCK : (len - off));
        if (n)
            mem_copy(raw, data + off, n);
        uint32_t neu = alloc_block();
        uint32_t old = 0;
        if (!neu || !write_block(neu, raw) || !map_set(ino, i, neu, &old))
            return false;
        if (old)
            free_block(old);
    }
    for (uint32_t i = new_blocks; i < old_blocks; ++i) {
        uint32_t old = 0;
        if (!map_set(ino, i, 0, &old))
            return false;
        if (old)
            free_block(old);
    }
    if (ino->type == 1) {
        if (len >= ino->size)
            used_live += len - ino->size;
        else
            used_live -= ino->size - len;
    }
    ino->size = len;
    return true;
}
static void pack_cached(const IBlock *c, uint8_t *raw) {
    mem_set(raw, 0, BLOCK);
    for (uint32_t s = 0; s < INODES_PER_BLOCK; ++s)
        pack_inode(&c->ent[s], raw + s * INODE_BYTES);
}
static bool inodes_dirty(void) {
    for (uint32_t i = 0; i < ICACHE; ++i)
        if (icache[i].used && icache[i].dirty)
            return true;
    return false;
}
static bool flush_inodes(bool cow) {
    uint8_t raw[BLOCK];
    for (uint32_t i = 0; i < ICACHE; ++i) {
        IBlock *c = &icache[i];
        if (!c->used || !c->dirty)
            continue;
        uint32_t dest = c->loc;
        uint32_t old = 0;
        if (cow && !c->fresh) {
            dest = alloc_block();
            if (!dest)
                return false;
            old = c->loc;
        }
        pack_cached(c, raw);
        if (!write_block(dest, raw))
            return false;
        if (old)
            free_block(old);
        c->loc = dest;
        c->fresh = 1;
    }
    return true;
}
static bool rewrite_index(bool cow) {
    uint32_t count = iblock_count ? iblock_count : 1u;
    uint32_t hops = (count + INDEX_DATA - 1u) / INDEX_DATA;
    uint32_t old = index_block;
    uint32_t new_head = 0;
    uint32_t prev_neu = 0;
    uint32_t prev_words[1024];
    int have_prev = 0;
    uint8_t raw[BLOCK];
    for (uint32_t h = 0; h < hops; ++h) {
        uint32_t words[1024];
        mem_set(words, 0, sizeof words);
        uint32_t next_old = 0;
        if (old) {
            load_words(old, words);
            next_old = words[1023];
        }
        words[1023] = 0;
        for (uint32_t slot = 0; slot < INDEX_DATA; ++slot) {
            uint32_t idx = h * INDEX_DATA + slot;
            if (idx >= count) {
                words[slot] = 0;
                continue;
            }
            IBlock *c = icache_find(idx);
            if (c)
                words[slot] = c->loc;
            else if (!words[slot])
                return false;
        }
        uint32_t neu;
        if (!cow && h == 0)
            neu = index_block;
        else {
            neu = alloc_block();
            if (!neu)
                return false;
        }
        if (!have_prev)
            new_head = neu;
        else {
            prev_words[1023] = neu;
            for (unsigned i = 0; i < 1024; ++i)
                put32(raw + i * 4, prev_words[i]);
            if (!write_block(prev_neu, raw))
                return false;
        }
        mem_copy(prev_words, words, sizeof words);
        prev_neu = neu;
        have_prev = 1;
        old = next_old;
    }
    for (unsigned i = 0; i < 1024; ++i)
        put32(raw + i * 4, prev_words[i]);
    if (!write_block(prev_neu, raw))
        return false;
    if (cow) {
        uint32_t link = index_block;
        while (link) {
            uint32_t words[1024];
            load_words(link, words);
            uint32_t next = words[1023];
            free_block(link);
            link = next;
        }
    }
    index_block = new_head;
    return true;
}
static void fill_super(uint8_t *sec, uint64_t gen, const uint32_t *bm, uint32_t index, uint32_t inodes) {
    mem_set(sec, 0, SECTOR);
    mem_copy(sec, "ARKFS2\0\0", 8);
    put32(sec + 8, 2);
    put32(sec + 12, BLOCK);
    put32(sec + 16, total_blocks);
    put32(sec + 20, journal_block);
    put32(sec + 24, JOURNAL_BLOCKS);
    put64(sec + 28, gen);
    put32(sec + 36, reserved_prefix);
    put32(sec + 40, inodes);
    put32(sec + 44, bitmap_count);
    put32(sec + 48, index);
    put64(sec + 68, used_live);
    for (uint32_t i = 0; i < MAX_BITMAP; ++i)
        put32(sec + 52 + i * 4, i < bitmap_count ? bm[i] : 0);
    put32(sec + 508, crc32(sec, 508));
}
static void fill_commit(uint8_t *sec, uint64_t gen, const uint32_t *bm, uint32_t index, uint32_t inodes) {
    mem_set(sec, 0, SECTOR);
    mem_copy(sec, "ARKCMT2\0", 8);
    put64(sec + 8, gen);
    put32(sec + 16, bitmap_count);
    put32(sec + 20, inodes);
    put32(sec + 24, index);
    for (uint32_t i = 0; i < MAX_BITMAP; ++i)
        put32(sec + 28 + i * 4, i < bitmap_count ? bm[i] : 0);
    put32(sec + 508, crc32(sec, 508));
}
static bool write_super_sector(const uint8_t *sec) {
#ifdef ARK_STORAGE_HOST_TEST
    if (fail_superblock > 0) {
        --fail_superblock;
        return false;
    }
#endif
    return disk->write(0, 1, sec);
}
static bool write_commit_sector(const uint8_t *sec) {
    return disk->write(journal_block * 8u, 1, sec);
}
static bool reload(void);
static bool commit_cow(void) {
    bool dirty = inodes_dirty();
    uint32_t new_bm[MAX_BITMAP];
    for (uint32_t i = 0; i < bitmap_count; ++i) {
        new_bm[i] = alloc_block();
        if (!new_bm[i]) {
            const char *saved = last_error;
            reload();
            last_error = saved;
            return false;
        }
    }
    if (dirty && (!flush_inodes(true) || !rewrite_index(true))) {
        reload();
        return false;
    }
    for (uint32_t i = 0; i < bitmap_count; ++i)
        free_block(bitmap_loc[i]);
    for (uint32_t i = 0; i < bitmap_count; ++i) {
        if (!write_block(new_bm[i], bitmap_mem + i * BLOCK)) {
            reload();
            return false;
        }
    }
    if (!disk->flush()) {
        reload();
        return false;
    }
    uint8_t sec[SECTOR];
    uint64_t gen = generation + 1;
    uint32_t inodes = iblock_count;
    fill_commit(sec, gen, new_bm, index_block, inodes);
    if (!write_commit_sector(sec) || !disk->flush()) {
        reload();
        return false;
    }
    fill_super(sec, gen, new_bm, index_block, inodes);
    if (!write_super_sector(sec) || !disk->flush()) {
        reload();
        return false;
    }
    for (uint32_t i = 0; i < bitmap_count; ++i)
        bitmap_loc[i] = new_bm[i];
    icache_clear();
    generation = gen;
    snapshot_bits();
    last_error = "";
    return true;
}
static bool publish_first(void) {
    if (!flush_inodes(false) || !rewrite_index(false))
        return false;
    for (uint32_t i = 0; i < bitmap_count; ++i)
        if (!write_block(bitmap_loc[i], bitmap_mem + i * BLOCK))
            return false;
    if (!disk->flush())
        return false;
    uint8_t sec[SECTOR];
    fill_commit(sec, 1, bitmap_loc, index_block, iblock_count);
    if (!write_commit_sector(sec) || !disk->flush())
        return false;
    fill_super(sec, 1, bitmap_loc, index_block, iblock_count);
    if (!write_super_sector(sec) || !disk->flush())
        return false;
    generation = 1;
    mounted = true;
    building = false;
    icache_clear();
    snapshot_bits();
    last_error = "";
    return true;
}
static bool super_ok(const uint8_t *sec) {
    return mem_eq(sec, "ARKFS2\0\0", 8) && get32(sec + 8) == 2 && get32(sec + 12) == BLOCK &&
           get32(sec + 508) == crc32(sec, 508);
}
static bool load_tree(const uint8_t *sec) {
    total_blocks = get32(sec + 16);
    journal_block = get32(sec + 20);
    generation = get64(sec + 28);
    reserved_prefix = get32(sec + 36);
    iblock_count = get32(sec + 40);
    bitmap_count = get32(sec + 44);
    index_block = get32(sec + 48);
    if (!total_blocks || total_blocks > disk->sectors / 8 || bitmap_count == 0 ||
        bitmap_count > MAX_BITMAP || iblock_count == 0 || iblock_count > total_blocks ||
        journal_block >= total_blocks || index_block >= total_blocks)
        return false;
    mem_set(bitmap_mem, 0, sizeof bitmap_mem);
    for (uint32_t i = 0; i < bitmap_count; ++i) {
        bitmap_loc[i] = get32(sec + 52 + i * 4);
        if (!bitmap_loc[i] || bitmap_loc[i] >= total_blocks || !read_block(bitmap_loc[i], bitmap_mem + i * BLOCK))
            return false;
    }
    icache_clear();
    used_live = get64(sec + 68);
    {
        IBlock *root = hold_iblock(0);
        if (!root || root->ent[1].type != 2)
            return false;
    }
    mem_set(committed_mem, 0, sizeof committed_mem);
    mem_set(quarantine_mem, 0, sizeof quarantine_mem);
    mem_copy(committed_mem, bitmap_mem, sizeof committed_mem);
    return true;
}
static bool reload(void) {
    uint8_t sec[SECTOR];
    mounted = false;
    if (!disk || !disk->read(0, 1, sec) || !super_ok(sec) || !load_tree(sec))
        return false;
    mounted = true;
    building = false;
    return true;
}
static bool ready(void) {
    return mounted || building;
}
static bool finish(bool ok) {
    if (!ok) {
        const char *saved = last_error;
        if (mounted)
            reload();
        if (saved && saved[0])
            last_error = saved;
        return false;
    }
    if (building)
        return true;
    return commit_cow();
}
static bool layout(uint32_t prefix) {
    if (!disk || disk->sectors < 64)
        return false;
    mem_set(bitmap_mem, 0, sizeof bitmap_mem);
    mem_set(committed_mem, 0, sizeof committed_mem);
    mem_set(quarantine_mem, 0, sizeof quarantine_mem);
    icache_clear();
    used_live = 0;
    total_blocks = disk->sectors / 8u;
    bitmap_count = (total_blocks + 32767u) / 32768u;
    if (!bitmap_count)
        bitmap_count = 1;
    if (bitmap_count > MAX_BITMAP) {
        last_error = "空间不足，未保存";
        return false;
    }
    uint32_t cursor = prefix ? prefix : 1u;
    reserved_prefix = prefix;
    journal_block = cursor;
    cursor += JOURNAL_BLOCKS;
    for (uint32_t i = 0; i < bitmap_count; ++i)
        bitmap_loc[i] = cursor++;
    index_block = cursor++;
    {
        uint32_t ib = cursor++;
        icache[0].used = 1;
        icache[0].dirty = 1;
        icache[0].fresh = 1;
        icache[0].index = 0;
        icache[0].loc = ib;
        icache[0].ent[1].type = 2;
    }
    iblock_count = 1;
    if (cursor >= total_blocks) {
        last_error = "空间不足，未保存";
        return false;
    }
    for (uint32_t b = 0; b < cursor; ++b)
        bit_set(b, true);
    generation = 0;
    building = true;
    mounted = false;
    return true;
}
static bool ensure_dir(const char *canon);
static bool add_file(const char *canon, const uint8_t *data, uint64_t len, bool overwrite, bool make_parent) {
    char parent[1024], name[256];
    uint32_t nlen = 0, pin = 0, ino = 0;
    if (!split_last(canon, parent, name, &nlen))
        return false;
    if (make_parent && !ensure_dir(parent))
        return false;
    if (!lookup_path(parent, &pin) || ino_ref(pin)->type != 2)
        return false;
    if (dir_walk(ino_ref(pin), name, nlen, &ino, 0, 0)) {
        if (ino_ref(ino)->type != 1)
            return false;
        if ((ino_ref(ino)->flags & 1u) && overwrite) {
            last_error = "当前文件只读";
            return false;
        }
    } else {
        ino = alloc_inode();
        if (!ino)
            return false;
        ino_ref(ino)->type = 1;
        mark_inode(ino);
        if (!dir_insert(ino_ref(pin), ino, name, nlen))
            return false;
        mark_inode(pin);
    }
    if (!overwrite)
        return true;
    if (!write_bytes(ino_ref(ino), data, len))
        return false;
    mark_inode(ino);
    return true;
}
static bool ensure_dir(const char *canon) {
    if (str_eq(canon, "/"))
        return ino_ref(1)->type == 2;
    uint32_t existing = 0;
    if (lookup_path(canon, &existing))
        return ino_ref(existing)->type == 2;
    char parent[1024], name[256];
    uint32_t nlen = 0, pin = 0;
    if (!split_last(canon, parent, name, &nlen) || !ensure_dir(parent) || !lookup_path(parent, &pin))
        return false;
    uint32_t ino = alloc_inode();
    if (!ino)
        return false;
    ino_ref(ino)->type = 2;
    mark_inode(ino);
    if (!dir_insert(ino_ref(pin), ino, name, nlen))
        return false;
    mark_inode(pin);
    return true;
}

static uint8_t v1pay[1064960];
static struct {
    char name[128];
    uint32_t kind, size, off;
} v1tab[64];
static unsigned v1n;

static bool v1_bank(uint32_t lba, uint64_t *gen, uint32_t *length, uint32_t *paycrc, uint32_t *count) {
    uint8_t sec[SECTOR];
    if (!disk->read(lba, 1, sec))
        return false;
    if (!mem_eq(sec, "ARKBANK1", 8) || get32(sec + 28) != 1 || get32(sec + 32) != 0x41524b31u ||
        get32(sec + 508) != crc32(sec, 508))
        return false;
    *gen = get64(sec + 8);
    *length = get32(sec + 16);
    *paycrc = get32(sec + 20);
    *count = get32(sec + 24);
    return *gen > 0 && *count <= 64 && *length <= sizeof v1pay && *length >= *count * V1_RECORD;
}
static bool parse_v1(void) {
    uint8_t sec[SECTOR];
    v1n = 0;
    if (!disk->read(0, 1, sec) || !mem_eq(sec, "ARKFS1\0\0", 8) || get32(sec + 8) != 1 ||
        get32(sec + 508) != crc32(sec, 508))
        return false;
    int pick = -1;
    uint64_t best = 0, gen = 0;
    uint32_t length = 0, paycrc = 0, count = 0, lba = 0;
    for (int bank = 0; bank < 2; ++bank) {
        uint32_t at = bank ? V1_BANK1 : V1_BANK0;
        uint64_t g;
        uint32_t len, crc, n;
        if (!v1_bank(at, &g, &len, &crc, &n))
            continue;
        if (pick < 0 || g >= best) {
            pick = bank;
            best = g;
            gen = g;
            length = len;
            paycrc = crc;
            count = n;
            lba = at;
        }
    }
    (void)gen;
    if (pick < 0)
        return false;
    mem_set(v1pay, 0, sizeof v1pay);
    uint32_t sectors = (length + SECTOR - 1) / SECTOR;
    if (sectors && !disk->read(lba + 1, sectors, v1pay))
        return false;
    if (crc32(v1pay, length) != paycrc)
        return false;
    uint32_t off = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (length - off < V1_RECORD)
            return false;
        uint32_t kind = get32(v1pay + off), size = get32(v1pay + off + 4);
        if ((kind != 1 && kind != 2) || size >= 16384 || (kind == 2 && size) || size > length - off - V1_RECORD)
            return false;
        uint32_t n = 0;
        while (n < 128 && v1pay[off + 8 + n])
            ++n;
        if (n == 128 || v1pay[off + 8] != '/')
            return false;
        mem_set(v1tab[v1n].name, 0, 128);
        mem_copy(v1tab[v1n].name, v1pay + off + 8, n);
        v1tab[v1n].kind = kind;
        v1tab[v1n].size = size;
        v1tab[v1n].off = off + V1_RECORD;
        ++v1n;
        off += V1_RECORD + size;
    }
    return off == length;
}
static bool import_v1(void) {
    for (unsigned i = 0; i < v1n; ++i) {
        if (v1tab[i].kind != 2 || str_eq(v1tab[i].name, "/"))
            continue;
        char canon[1024];
        if (!canonicalize(v1tab[i].name, canon) || !ensure_dir(canon))
            return false;
    }
    for (unsigned i = 0; i < v1n; ++i) {
        if (v1tab[i].kind != 1)
            continue;
        char canon[1024];
        if (!canonicalize(v1tab[i].name, canon) ||
            !add_file(canon, v1pay + v1tab[i].off, v1tab[i].size, true, true))
            return false;
    }
    return true;
}

bool arkfs2_format(Arkfs2Disk *volume) {
    disk = volume;
    last_error = "";
    mounted = false;
    if (!layout(0) || !publish_first()) {
        building = false;
        mounted = false;
        if (!last_error[0])
            last_error = "空间不足，未保存";
        return false;
    }
    return true;
}
bool arkfs2_mount(Arkfs2Disk *volume) {
    disk = volume;
    last_error = "";
    return reload();
}
bool arkfs2_migrate_v1(Arkfs2Disk *volume) {
    disk = volume;
    last_error = "";
    mounted = false;
    building = false;
    if (!parse_v1())
        return false;
    if ((disk->sectors / 8u) <= (V1_PREFIX_SECTORS / 8u) + JOURNAL_BLOCKS + 4u)
        return false;
    if (!layout(V1_PREFIX_SECTORS / 8u) || !import_v1() || !publish_first()) {
        building = false;
        mounted = false;
        return false;
    }
    return true;
}
void arkfs2_unmount(void) {
    mounted = false;
    building = false;
}
const char *arkfs2_error(void) {
    return last_error;
}
uint64_t arkfs2_used_bytes(void) {
    return mounted ? used_live : 0;
}
uint64_t arkfs2_free_bytes(void) {
    uint64_t n = 0;
    if (!mounted)
        return 0;
    for (uint32_t b = 0; b < total_blocks; ++b)
        if (!bit_get(b))
            n += BLOCK;
    return n;
}
bool arkfs2_card(char *out, uint32_t cap) {
    if (!out || cap < 64 || !mounted)
        return false;
    uint64_t used = arkfs2_used_bytes(), free_bytes = arkfs2_free_bytes();
    char tmp[96];
    const char *labels[2] = {"已用 ", "剩余 "};
    uint64_t values[2] = {used, free_bytes};
    uint32_t pos = 0;
    for (int line = 0; line < 2; ++line) {
        const char *label = labels[line];
        uint32_t i = 0;
        while (label[i])
            tmp[i] = label[i], ++i;
        char num[24];
        uint64_t v = values[line];
        int k = 0;
        if (!v)
            num[k++] = '0';
        else {
            char rev[24];
            int r = 0;
            while (v) {
                rev[r++] = (char)('0' + (v % 10));
                v /= 10;
            }
            while (r)
                num[k++] = rev[--r];
        }
        for (int t = 0; t < k; ++t)
            tmp[i++] = num[t];
        const char *tail = " 字节";
        for (int t = 0; tail[t]; ++t)
            tmp[i++] = tail[t];
        if (line == 0)
            tmp[i++] = '\n';
        tmp[i] = 0;
        for (uint32_t t = 0; tmp[t]; ++t) {
            if (pos + 1 >= cap)
                return false;
            out[pos++] = tmp[t];
        }
    }
    out[pos] = 0;
    return true;
}
bool arkfs2_mkdir(const char *path) {
    char canon[1024];
    if (!ready() || !canonicalize(path, canon))
        return false;
    uint32_t existing = 0;
    if (lookup_path(canon, &existing))
        return false;
    return finish(ensure_dir(canon));
}
bool arkfs2_write(const char *path, const void *data, uint64_t len) {
    char canon[1024];
    if (!ready() || !canonicalize(path, canon))
        return false;
    if (!data && len)
        return false;
    return finish(add_file(canon, data, len, true, false));
}
bool arkfs2_read(const char *path, void *data, uint64_t cap, uint64_t *out_len) {
    char canon[1024];
    uint32_t ino = 0;
    if (!mounted || !canonicalize(path, canon) || !lookup_path(canon, &ino) || ino_ref(ino)->type != 1)
        return false;
    if (out_len)
        *out_len = ino_ref(ino)->size;
    if (ino_ref(ino)->size > cap)
        return false;
    uint8_t *dst = data;
    uint64_t left = ino_ref(ino)->size;
    uint32_t logical = 0;
    while (left) {
        uint8_t raw[BLOCK];
        uint32_t id = map_get(ino_ref(ino), logical++);
        mem_set(raw, 0, sizeof raw);
        if (id && !read_block(id, raw))
            return false;
        uint32_t n = left > BLOCK ? BLOCK : (uint32_t)left;
        mem_copy(dst, raw, n);
        dst += n;
        left -= n;
    }
    return true;
}
bool arkfs2_rename(const char *from, const char *to) {
    char a[1024], b[1024];
    if (!mounted || !canonicalize(from, a) || !canonicalize(to, b))
        return false;
    if (str_eq(a, "/") || str_eq(b, "/")) {
        last_error = "名称不能为空";
        return false;
    }
    uint32_t ino = 0, src_parent = 0, dst_parent = 0;
    char src_name[256], dst_name[256], src_path[1024], dst_path[1024];
    uint32_t slen = 0, dlen = 0;
    if (!lookup_path(a, &ino) || !split_last(a, src_path, src_name, &slen) ||
        !split_last(b, dst_path, dst_name, &dlen) || !lookup_path(src_path, &src_parent) ||
        !lookup_path(dst_path, &dst_parent))
        return false;
    uint32_t clash = 0;
    if (dir_walk(ino_ref(dst_parent), dst_name, dlen, &clash, 0, 0))
        return false;
    bool ok = dir_remove(ino_ref(src_parent), src_name, slen) &&
              dir_insert(ino_ref(dst_parent), ino, dst_name, dlen);
    if (ok) {
        mark_inode(src_parent);
        mark_inode(dst_parent);
    }
    return finish(ok);
}
bool arkfs2_set_readonly(const char *path, bool on) {
    char canon[1024];
    uint32_t ino = 0;
    if (!mounted || !canonicalize(path, canon) || !lookup_path(canon, &ino) || ino_ref(ino)->type != 1)
        return false;
    if (on)
        ino_ref(ino)->flags |= 1u;
    else
        ino_ref(ino)->flags &= ~1u;
    mark_inode(ino);
    return finish(true);
}
bool arkfs2_meta(uint32_t *bitmap_block, uint32_t *inode_block, uint64_t *gen) {
    if (!mounted)
        return false;
    if (bitmap_block)
        *bitmap_block = bitmap_loc[0];
    if (inode_block) {
        uint32_t loc = 0;
        if (!index_lookup(0, &loc))
            return false;
        *inode_block = loc;
    }
    if (gen)
        *gen = generation;
    return true;
}
bool arkfs2_block(const char *path, uint32_t logical, uint32_t *block_id) {
    char canon[1024];
    uint32_t ino = 0;
    if (!mounted || !block_id || !canonicalize(path, canon) || !lookup_path(canon, &ino))
        return false;
    *block_id = map_get(ino_ref(ino), logical);
    return true;
}
bool arkfs2_lookup(const char *path, uint32_t *ino, uint64_t *size, uint32_t *type) {
    char canon[1024];
    uint32_t id = 0;
    if (!mounted || !canonicalize(path, canon) || !lookup_path(canon, &id))
        return false;
    if (ino)
        *ino = id;
    if (size)
        *size = ino_ref(id)->size;
    if (type)
        *type = ino_ref(id)->type;
    return true;
}
#ifdef ARK_STORAGE_HOST_TEST
void arkfs2_fail_superblock(int times) {
    fail_superblock = times;
}
uint32_t arkfs2_inode_resident(void) {
    return (uint32_t)sizeof icache;
}
uint32_t arkfs2_inode_loads(void) {
    return inode_loads;
}
void arkfs2_reset_inode_loads(void) {
    inode_loads = 0;
}
#endif
