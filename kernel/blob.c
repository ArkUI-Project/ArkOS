/* ArkFS binary store: growable COW index pages and two committed roots.
 * Old ARKBLOB1 indices are read directly; the next successful transaction uses
 * ARKBLOB2 while retaining the old generation and all of its live extents. */
#define ARK_KERNEL
#include "ark_api.h"
#include "ark.h"
#include "process.h"
#include "storage.h"
#include "accounts.h"
#include "blob.h"
#include "alloc.h"
#define INDEX_LBA 8192u
#define ARENA_LBA 8208u
#define RECORD_SIZE 96u
#define PAGE_RECORDS 42u
typedef struct Record {
    struct Record *next;
    uint32_t uid, start, len, checksum;
    char name[64];
} Record;
typedef struct Page {
    struct Page *next;
    uint32_t start;
} Page;
typedef struct {
    bool valid;
    uint64_t generation;
    unsigned count;
    Record *records;
    Page *pages;
} Bank;
static Bank banks[2];
static uint8_t root[4096], page_data[4096], io[65536];
static bool loaded;
static unsigned active_index;
static uint32_t rd(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t rd64(const uint8_t *p) {
    return rd(p) | (uint64_t)rd(p + 4) << 32;
}
static void wr(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(n >> (i * 8));
}
static void wr64(uint8_t *p, uint64_t n) {
    wr(p, (uint32_t)n);
    wr(p + 4, (uint32_t)(n >> 32));
}
static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u,
    0x9e6495a3u, 0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu,
    0xe7b82d07u, 0x90bf1d91u, 0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du,
    0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u, 0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu,
    0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u, 0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u,
    0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu, 0x35b5a8fau, 0x42b2986cu,
    0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u, 0x26d930acu,
    0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu,
    0xb6662d3du, 0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu,
    0x9fbfe4a5u, 0xe8b8d433u, 0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu,
    0x086d3d2du, 0x91646c97u, 0xe6635c01u, 0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu,
    0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u, 0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau,
    0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u, 0x4db26158u, 0x3ab551ceu,
    0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu, 0x4369e96au,
    0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u,
    0xce61e49fu, 0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u,
    0xb7bd5c3bu, 0xc0ba6cadu, 0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u,
    0x9dd277afu, 0x04db2615u, 0x73dc1683u, 0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u,
    0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u, 0xf00f9344u, 0x8708a3d2u, 0x1e01f268u,
    0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u, 0xfed41b76u, 0x89d32be0u,
    0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u, 0xd6d6a3e8u,
    0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu,
    0x4669be79u, 0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u,
    0x220216b9u, 0x5505262fu, 0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u,
    0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du, 0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au,
    0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u, 0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu,
    0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u, 0x86d3d2d4u, 0xf1d4e242u,
    0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u, 0x88085ae6u,
    0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du,
    0x3e6e77dbu, 0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u,
    0x47b2cf7fu, 0x30b5ffe9u, 0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u,
    0xcdd70693u, 0x54de5729u, 0x23d967bfu, 0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u,
    0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du};
static uint32_t update(uint32_t c, const uint8_t *p, size_t n) {
    while (n--)
        c = (c >> 8) ^ crc_table[(c ^ *p++) & 255];
    return c;
}
static uint32_t crc(const uint8_t *p, size_t n) {
    return ~update(~0u, p, n);
}

static bool name_ok(const char *s) {
    unsigned n = 0;
    while (n < 64 && s[n]) {
        unsigned c = (uint8_t)s[n++];
        if (c < 32 || c == '/' || c == '\\')
            return false;
    }
    return n > 0 && n < 64 && strcmp(s, ".") && strcmp(s, "..");
}
static void clear(Bank *b) {
    while (b->records) {
        Record *r = b->records;
        b->records = r->next;
        ark_free(r);
    }
    while (b->pages) {
        Page *p = b->pages;
        b->pages = p->next;
        ark_free(p);
    }
    memset(b, 0, sizeof *b);
}
static bool overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m) {
    return (uint64_t)a < b + (uint64_t)m && (uint64_t)b < a + (uint64_t)n;
}
static bool extent(uint32_t start, uint32_t sectors) {
    uint32_t size = storage_volume_sectors();
    return start >= ARENA_LBA && start < size && sectors && sectors <= size - start;
}
static bool add_record(Bank *b, const uint8_t *data) {
    uint32_t uid = rd(data), start = rd(data + 4), len = rd(data + 8);
    if (!uid)
        return true;
    if (uid < 1000 || !name_ok((const char *)data + 24) || !len || len > ARK_BLOB_MAX ||
        !extent(start, (len + 511) / 512))
        return false;
    for (Record *r = b->records; r; r = r->next)
        if (overlap(start, (len + 511) / 512, r->start, (r->len + 511) / 512) ||
            (r->uid == uid && !strcmp(r->name, (const char *)data + 24)))
            return false;
    Record *r = ark_alloc(sizeof *r);
    if (!r)
        return false;
    *r = (Record){
        .next = b->records, .uid = uid, .start = start, .len = len, .checksum = rd(data + 12)};
    strcopy(r->name, (const char *)data + 24, 64);
    b->records = r;
    b->count++;
    return true;
}
static bool read_bank(unsigned i) {
    Bank *b = &banks[i];
    if (!storage_volume_io(INDEX_LBA + i * 8, 8, root, false))
        return false;
    if (rd(root + 4092) != crc(root, 4092) || !rd64(root + 8))
        return false;
    b->generation = rd64(root + 8);
    if (!memcmp(root, "ARKBLOB1", 8)) {
        for (unsigned j = 0; j < 16; j++)
            if (!add_record(b, root + 32 + j * 96))
                return false;
    } else if (!memcmp(root, "ARKBLOB2", 8)) {
        uint32_t count = rd(root + 20), next = rd(root + 16), seen = 0;
        if (count > (storage_volume_sectors() - ARENA_LBA))
            return false;
        unsigned pages = (count + PAGE_RECORDS - 1) / PAGE_RECORDS;
        for (unsigned j = 0; j < pages; j++) {
            if (!extent(next, 8))
                return false;
            for (Page *p = b->pages; p; p = p->next)
                if (p->start == next)
                    return false;
            Page *p = ark_alloc(sizeof *p);
            if (!p)
                return false;
            *p = (Page){b->pages, next};
            b->pages = p;
            if (!storage_volume_io(next, 8, page_data, false) || memcmp(page_data, "ARKIDX2", 8) ||
                rd(page_data + 4092) != crc(page_data, 4092))
                return false;
            unsigned n = rd(page_data + 12);
            if (!n || n > PAGE_RECORDS || n > count - seen)
                return false;
            for (unsigned k = 0; k < n; k++)
                if (!rd(page_data + 32 + k * 96) || !add_record(b, page_data + 32 + k * 96))
                    return false;
            seen += n;
            next = rd(page_data + 8);
        }
        if (seen != count || next)
            return false;
    } else
        return false;
    for (Page *p = b->pages; p; p = p->next) {
        for (Page *q = p->next; q; q = q->next)
            if (overlap(p->start, 8, q->start, 8))
                return false;
        for (Record *r = b->records; r; r = r->next)
            if (overlap(p->start, 8, r->start, (r->len + 511) / 512))
                return false;
    }
    b->valid = true;
    return true;
}
static bool load(void) {
    if (loaded)
        return true;
    if (!storage_mounted() || storage_volume_sectors() < ARENA_LBA + 128)
        return false;
    bool blank = true;
    for (unsigned i = 0; i < 2; i++) {
        if (!read_bank(i)) {
            clear(&banks[i]);
            if (!storage_volume_io(INDEX_LBA + i * 8, 8, root, false))
                return false;
            for (unsigned n = 0; n < 4096; n++)
                if (root[n])
                    blank = false;
        }
    }
    if (!banks[0].valid && !banks[1].valid && !blank)
        return false;
    active_index =
        banks[1].valid && (!banks[0].valid || banks[1].generation > banks[0].generation) ? 1 : 0;
    loaded = true;
    return true;
}
static uint32_t allocate_extent(unsigned sectors, const Bank *pending) {
    uint32_t start = ARENA_LBA;
    bool again = true;
    while (again) {
        again = false;
        for (unsigned i = 0; i < 3; i++) {
            const Bank *b = i == 2 ? pending : &banks[i];
            if (!b)
                continue;
            for (Record *r = b->records; r; r = r->next)
                if (overlap(start, sectors, r->start, (r->len + 511) / 512)) {
                    start = r->start + (r->len + 511) / 512;
                    again = true;
                }
            for (Page *p = b->pages; p; p = p->next)
                if (overlap(start, sectors, p->start, 8)) {
                    start = p->start + 8;
                    again = true;
                }
        }
        if (!extent(start, sectors))
            return 0;
    }
    return start;
}
static bool copy_records(Bank *out, const Bank *in) {
    Record **tail = &out->records;
    for (Record *r = in->records; r; r = r->next) {
        Record *n = ark_alloc(sizeof *n);
        if (!n)
            return false;
        *n = *r;
        n->next = 0;
        *tail = n;
        tail = &n->next;
        out->count++;
    }
    return true;
}
static bool commit(Bank *next) {
    uint64_t gen = banks[active_index].generation;
    if (gen == UINT64_MAX)
        return false;
    unsigned pages = (next->count + PAGE_RECORDS - 1) / PAGE_RECORDS;
    Page **tail = &next->pages;
    for (unsigned i = 0; i < pages; i++) {
        uint32_t start = allocate_extent(8, next);
        Page *p = ark_alloc(sizeof *p);
        if (!start || !p) {
            ark_free(p);
            return false;
        }
        *p = (Page){0, start};
        *tail = p;
        tail = &p->next;
    }
    Record *r = next->records;
    for (Page *p = next->pages; p; p = p->next) {
        memset(page_data, 0, sizeof page_data);
        memcpy(page_data, "ARKIDX2", 8);
        wr(page_data + 8, p->next ? p->next->start : 0);
        unsigned n = 0;
        while (r && n < PAGE_RECORDS) {
            uint8_t *data = page_data + 32 + n++ * 96;
            wr(data, r->uid);
            wr(data + 4, r->start);
            wr(data + 8, r->len);
            wr(data + 12, r->checksum);
            strcopy((char *)data + 24, r->name, 64);
            r = r->next;
        }
        wr(page_data + 12, n);
        wr(page_data + 4092, crc(page_data, 4092));
        if (!storage_volume_io(p->start, 8, page_data, true))
            return false;
    }
    memset(root, 0, sizeof root);
    memcpy(root, "ARKBLOB2", 8);
    wr64(root + 8, gen + 1);
    wr(root + 16, next->pages ? next->pages->start : 0);
    wr(root + 20, next->count);
    wr(root + 4092, crc(root, 4092));
    unsigned target = 1 - active_index;
    if (!storage_volume_flush() || !storage_volume_io(INDEX_LBA + target * 8, 8, root, true) ||
        !storage_volume_flush())
        return false;
    clear(&banks[target]);
    next->generation = gen + 1;
    next->valid = true;
    banks[target] = *next;
    memset(next, 0, sizeof *next);
    active_index = target;
    return true;
}
static int64_t blob_access(ArkBlobRequest *q, uint32_t uid, bool kernel) {
    if (!load()) {
        strcopy(q->error, "无法打开文件存储，请检查系统磁盘", sizeof q->error);
        return -5;
    }
    Bank *b = &banks[active_index];
    Record *found = 0;
    unsigned count = 0;
    for (Record *r = b->records; r; r = r->next) {
        if (r->uid != uid ||
            (!kernel && (!strncmp(r->name, "@pkg.", 5) || !strncmp(r->name, "@registry.", 10) ||
                         !strncmp(r->name, "@swap.", 6))))
            continue;
        if (q->op == ARK_BLOB_LIST) {
            if (count++ == q->index) {
                found = r;
                strcopy(q->name, r->name, sizeof q->name);
                q->size = r->len;
                q->checksum = r->checksum;
            }
        } else if (name_ok(q->name) && !strcmp(q->name, r->name))
            found = r;
    }
    if (q->op == ARK_BLOB_LIST) {
        q->count = count;
        return found ? 0 : -2;
    }
    if (!name_ok(q->name))
        return -22;
    if (q->op == ARK_BLOB_WRITE || q->op == ARK_BLOB_REMOVE) {
        if (q->op == ARK_BLOB_REMOVE && !found)
            return -2;
        if (q->op == ARK_BLOB_WRITE &&
            (!q->capacity || q->capacity > ARK_BLOB_MAX ||
             (!kernel && !process_user_range(q->buffer, q->capacity, false))))
            return -14;
        Bank next = {0};
        if (!copy_records(&next, b)) {
            clear(&next);
            return -12;
        }
        Record **where = &next.records;
        while (*where && ((*where)->uid != uid || strcmp((*where)->name, q->name)))
            where = &(*where)->next;
        if (q->op == ARK_BLOB_REMOVE) {
            Record *r = *where;
            *where = r->next;
            ark_free(r);
            next.count--;
        } else {
            uint32_t start = allocate_extent((q->capacity + 511) / 512, &next);
            if (!start) {
                clear(&next);
                return -28;
            }
            uint32_t c = ~0u;
            for (unsigned at = 0; at < q->capacity;) {
                unsigned n = q->capacity - at;
                if (n > sizeof io)
                    n = sizeof io;
                memset(io, 0, (n + 511) & ~511u);
                if (kernel)
                    memcpy(io, (const void *)(uintptr_t)(q->buffer + at), n);
                else if (!process_copy_from_user(io, q->buffer + at, n)) {
                    clear(&next);
                    return -14;
                }
                c = update(c, io, n);
                if (!storage_volume_io(start + at / 512, (n + 511) / 512, io, true)) {
                    clear(&next);
                    return -5;
                }
                at += n;
            }
            if (!*where) {
                *where = ark_alloc(sizeof **where);
                if (!*where) {
                    clear(&next);
                    return -12;
                }
                next.count++;
            }
            Record *r = *where;
            r->uid = uid;
            r->start = start;
            r->len = q->capacity;
            r->checksum = ~c;
            strcopy(r->name, q->name, 64);
            q->size = q->capacity;
            q->checksum = ~c;
            q->count = q->capacity;
        }
        bool ok = commit(&next);
        clear(&next);
        return ok ? 0 : -5;
    }
    if (!found)
        return -2;
    q->size = found->len;
    q->checksum = found->checksum;
    if (q->op == ARK_BLOB_READ) {
        if (q->offset > q->size || q->capacity > q->size - q->offset ||
            (!kernel && !process_user_range(q->buffer, q->capacity, true)))
            return -14;
        uint32_t c = ~0u;
        for (unsigned at = 0; at < q->size;) {
            unsigned n = q->size - at;
            if (n > sizeof io)
                n = sizeof io;
            if (!storage_volume_io(found->start + at / 512, (n + 511) / 512, io, false))
                return -5;
            c = update(c, io, n);
            unsigned lo = at > q->offset ? at : q->offset,
                     hi = at + n < q->offset + q->capacity ? at + n : q->offset + q->capacity;
            if (lo < hi) {
                if (kernel)
                    memcpy((void *)(uintptr_t)(q->buffer + lo - q->offset), io + lo - at, hi - lo);
                else if (!process_copy_to_user(q->buffer + lo - q->offset, io + lo - at, hi - lo))
                    return -14;
            }
            at += n;
        }
        if (~c != q->checksum)
            return -5;
        q->count = q->capacity;
        return 0;
    }
    return -22;
}
int64_t blob_kernel_request(ArkBlobRequest *q, uint32_t uid) {
    return uid < 1000 ? -1 : blob_access(q, uid, true);
}
int64_t blob_request(ArkBlobRequest *q) {
    uint32_t uid = process_has_cap(ARK_CAP_SYSTEM) ? accounts_current_uid() : process_current_uid();
    if (!process_has_cap(ARK_CAP_FILES) || accounts_state() != ACCOUNT_ACTIVE ||
        uid != accounts_current_uid())
        return -1;
    if (q->op != ARK_BLOB_LIST &&
        (!strncmp(q->name, "@pkg.", 5) || !strncmp(q->name, "@registry.", 10) ||
         !strncmp(q->name, "@swap.", 6)))
        return -1;
    return blob_access(q, uid, false);
}
/* Scratch backing survives a reboot as allocated private extents, but live
 * slot ownership and checksums belong to this boot only. Each slot is written
 * before it is referenced by a PTE; old bytes are never read as a new page. */
bool blob_swap_io(unsigned slot, void *data, bool write) {
    if (slot >= 16384 || !data || !load())
        return false;
    unsigned chunk = slot / 32;
    char name[64] = "@swap.", number[4];
    unsigned value = chunk, n = 0;
    do {
        number[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    for (unsigned i = 0; i < n; i++)
        name[6 + i] = number[n - i - 1];
    name[6 + n] = 0;
    Record *found = 0;
    for (Record *r = banks[active_index].records; r; r = r->next)
        if (r->uid == 1000 && !strcmp(r->name, name)) {
            found = r;
            break;
        }
    if (!found) {
        if (!write)
            return false;
        Bank next = {0};
        if (!copy_records(&next, &banks[active_index])) {
            clear(&next);
            return false;
        }
        uint32_t start = allocate_extent(256, &next);
        Record *r = start ? ark_alloc(sizeof *r) : 0;
        if (!r) {
            clear(&next);
            return false;
        }
        *r = (Record){.next = next.records, .uid = 1000, .start = start, .len = 128 * 1024};
        strcopy(r->name, name, 64);
        next.records = r;
        next.count++;
        if (!commit(&next)) {
            clear(&next);
            return false;
        }
        for (Record *q = banks[active_index].records; q; q = q->next)
            if (q->uid == 1000 && !strcmp(q->name, name)) {
                found = q;
                break;
            }
    }
    if (!found || found->len != 128 * 1024)
        return false;
    return storage_volume_io(found->start + (slot % 32) * 8, 8, data, write) &&
           (write ? storage_volume_flush() : true);
}
uint64_t blob_swap_capacity(void) {
    uint64_t n = 0;
    if (loaded)
        for (Record *r = banks[active_index].records; r; r = r->next)
            if (r->uid == 1000 && !strncmp(r->name, "@swap.", 6) && r->len == 128 * 1024)
                n += r->len;
    return n;
}
#ifdef ARK_BLOB_HOST_TEST
void blob_test_reset(void) {
    clear(&banks[0]);
    clear(&banks[1]);
    loaded = false;
}
#endif
