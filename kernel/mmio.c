/* Bounded native x86-64 MMIO mappings for firmware-assigned PCI BARs.
 * Boot RAM/page tables remain in the initial 4 GiB identity mapping. */
#include "mmio.h"
#define PAGE_2M 0x200000ull
#define TABLE_MASK 0x000ffffffffff000ull
#define LARGE_MASK 0x000fffffffe00000ull
#define POOL_PAGES 32u
static uint64_t table_pool[POOL_PAGES][512] __attribute__((aligned(4096)));
static unsigned tables_used;
static bool reported_high;
static uint64_t *new_table(void) {
    if (tables_used == POOL_PAGES)
        return 0;
    uint64_t *table = table_pool[tables_used++];
    memset(table, 0, 4096);
    return table;
}
static uint64_t *next_table(uint64_t *entry) {
    if (*entry & 1) {
        if (*entry & 0x80)
            return 0; /* Never reinterpret an existing large-page leaf. */
        uint64_t physical = *entry & TABLE_MASK;
        if (physical >= 0x100000000ull)
            return 0; /* All our table pages are low RAM. */
        return (uint64_t *)(uintptr_t)physical;
    }
    uint64_t *table = new_table();
    if (!table)
        return 0;
    if ((uint64_t)(uintptr_t)table >= 0x100000000ull)
        return 0;
    *entry = (uint64_t)(uintptr_t)table | 3;
    return table;
}
bool platform_map_mmio(uint64_t physical, uint64_t bytes) {
    if (!bytes || bytes > 16 * 1024 * 1024ull || physical < 0x100000ull ||
        physical >= 0x0000800000000000ull || bytes > 0x0000800000000000ull - physical)
        return false;
    uint64_t start = physical & ~(PAGE_2M - 1),
             end = (physical + bytes + PAGE_2M - 1) & ~(PAGE_2M - 1);
    uint64_t flags, cr3;
    __asm__ volatile("pushfq; popq %0; cli; mov %%cr3,%1" : "=r"(flags), "=r"(cr3)::"memory");
    bool okay = true, changed = false, uncache_existing = false;
    uint64_t root = cr3 & TABLE_MASK;
    if (root >= 0x100000000ull)
        okay = false;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)root;
    for (uint64_t page = start; okay && page < end; page += PAGE_2M) {
        uint64_t *pdpt = next_table(&pml4[(page >> 39) & 511]);
        if (!pdpt) {
            okay = false;
            break;
        }
        uint64_t *pd = next_table(&pdpt[(page >> 30) & 511]);
        if (!pd) {
            okay = false;
            break;
        }
        uint64_t *leaf = &pd[(page >> 21) & 511];
        if ((*leaf & 1) && (!(*leaf & 0x80) || (*leaf & LARGE_MASK) != page)) {
            okay = false;
            break;
        }
        uint64_t wanted = (page & LARGE_MASK) | 0x9b; /* P,RW,PWT,PCD,PS; no user access. */
        if ((*leaf & ~0x60ull) != wanted) {
            if ((*leaf & 1) && ((*leaf & 0x18) != 0x18))
                uncache_existing = true;
            *leaf = wanted;
            changed = true;
        }
    }
    /* Updating a previously cacheable identity leaf requires both cache and TLB
     * invalidation. New high windows have no cached alias in this kernel. */
    if (uncache_existing)
        __asm__ volatile("wbinvd" ::: "memory");
    if (changed)
        __asm__ volatile("mov %0,%%cr3" ::"r"(cr3) : "memory");
    if (flags & (1u << 9))
        __asm__ volatile("sti" ::: "memory");
    if (okay && physical >= 0x100000000ull && !reported_high) {
        reported_high = true;
        serial_write("[mmio] Firmware high PCI window mapped with uncached 2 MiB pages\n");
    }
    return okay;
}
