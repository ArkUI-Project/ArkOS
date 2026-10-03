/* Original ArkOS anonymous virtual memory. Page tables remain private, data is
 * RW/NX, and paging never evicts an address space running on another CPU. */
#define VM_FIRST 0x60000000ull
#define VM_END 0x7e000000ull
#define VM_ANON (1ull << 11)
#define VM_COMMIT (1ull << 10)
#define VM_SWAP (1ull << 9)
#define VM_SLOTS 16384u
#define VM_REGIONS 128u
extern bool blob_swap_io(unsigned, void *, bool) __attribute__((weak));
extern uint64_t blob_swap_capacity(void) __attribute__((weak));
typedef struct {
    uint64_t base, bytes;
} VMRegion;
static VMRegion vm_regions[PROCESS_MAX][VM_REGIONS];
static struct {
    uint64_t reserved, committed, resident, swapped, faults, ins, outs;
} vm_usage[PROCESS_MAX];
static uint32_t page_va[POOL_PAGES], swap_va[VM_SLOTS], swap_hash[VM_SLOTS];
static uint8_t swap_owner[VM_SLOTS];
static unsigned swap_hint, reclaim_hint;
static int fault_owner = -1;
static uint32_t vm_hash(const void *p) {
    const uint32_t *v = p;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < 1024; i++)
        h = (h ^ v[i]) * 16777619u;
    return h;
}
static bool vm_remote_active(unsigned owner) {
    unsigned here = platform_current_cpu();
    for (unsigned c = 0; c < CPU_LIMIT; c++) {
        int t = cpu_state[c].task;
        if (c != here && t >= 0 && tasks[t].owner == owner)
            return true;
    }
    return false;
}
static void vm_invalidate(unsigned owner, uint64_t va) {
    if (current >= 0 && tasks[current].owner == owner)
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
}
static void vm_free_leaf(unsigned owner, uint64_t va, uint64_t *leaf) {
    if (*leaf & PTE_PRESENT) {
        unsigned i = (unsigned)(((*leaf & PAGE_MASK) - POOL_BASE) / 4096);
        page_owner[i] = 0;
        page_va[i] = 0;
        free_pages++;
        vm_usage[owner].resident -= 4096;
        tasks[owner].info.mapped_bytes -= 4096;
        if (i < free_hint)
            free_hint = i;
    } else if (*leaf & VM_SWAP) {
        unsigned slot = (unsigned)((*leaf & PAGE_MASK) / 4096) - 1;
        swap_owner[slot] = 0;
        swap_va[slot] = swap_hash[slot] = 0;
        vm_usage[owner].swapped -= 4096;
    }
    if (*leaf & VM_COMMIT)
        vm_usage[owner].committed -= 4096;
    *leaf = VM_ANON | PTE_USER | PTE_WRITE | PTE_NX;
    vm_invalidate(owner, va);
}
static void vm_release_owner(unsigned owner) {
    for (unsigned i = 0; i < VM_SLOTS; i++)
        if (swap_owner[i] == owner + 1) {
            swap_owner[i] = 0;
            swap_va[i] = swap_hash[i] = 0;
        }
    for (unsigned i = 0; i < pool_pages; i++)
        if (page_owner[i] == owner + 1)
            page_va[i] = 0;
    memset(vm_regions[owner], 0, sizeof vm_regions[owner]);
    memset(&vm_usage[owner], 0, sizeof vm_usage[owner]);
}
static bool vm_evict(unsigned owner, uint64_t va, uint64_t *leaf) {
    if (!blob_swap_io || vm_remote_active(owner) || !(*leaf & PTE_PRESENT) || !(*leaf & VM_ANON))
        return false;
    unsigned slot = VM_SLOTS;
    for (unsigned i = 0; i < VM_SLOTS; i++) {
        unsigned n = (swap_hint + i) % VM_SLOTS;
        if (!swap_owner[n]) {
            slot = n;
            break;
        }
    }
    if (slot == VM_SLOTS)
        return false;
    uint64_t old = *leaf;
    void *data = (void *)(uintptr_t)(old & PAGE_MASK);
    uint32_t hash = vm_hash(data);
    /* Failed writes leave the resident PTE and every byte intact. */
    if (!blob_swap_io(slot, data, true))
        return false;
    swap_owner[slot] = (uint8_t)(owner + 1);
    swap_va[slot] = (uint32_t)va;
    swap_hash[slot] = hash;
    swap_hint = (slot + 1) % VM_SLOTS;
    *leaf = ((uint64_t)(slot + 1) * 4096) |
            (old & ~(PAGE_MASK | PTE_PRESENT | (1ull << 5) | (1ull << 6))) | VM_SWAP;
    vm_invalidate(owner, va);
    unsigned page = (unsigned)(((old & PAGE_MASK) - POOL_BASE) / 4096);
    page_va[page] = 0;
    page_owner[page] = 0;
    free_pages++;
    if (page < free_hint)
        free_hint = page;
    vm_usage[owner].resident -= 4096;
    vm_usage[owner].swapped += 4096;
    vm_usage[owner].outs++;
    tasks[owner].info.mapped_bytes -= 4096;
    return true;
}
static bool vm_reclaim_one(void) {
    if (!initialized || !blob_swap_io)
        return false;
    for (unsigned pass = 0; pass < 2; pass++)
        for (unsigned i = 0; i < pool_pages; i++) {
            unsigned n = (reclaim_hint + i) % pool_pages, owner = (unsigned)page_owner[n] - 1;
            if (owner >= PROCESS_MAX || !page_va[n] || vm_remote_active(owner))
                continue;
            /* Syscall copies/aliases pin the caller. A ring3 fault may evict its own
             * cold anonymous page because no raw user alias has yet been acquired. */
            if (current >= 0 && tasks[current].owner == owner && fault_owner != (int)owner)
                continue;
            uint64_t *leaf = user_leaf(owner, page_va[n], false);
            if (!leaf || !(*leaf & PTE_PRESENT))
                continue;
            if (*leaf & (1ull << 5)) {
                *leaf &= ~(1ull << 5);
                vm_invalidate(owner, page_va[n]);
                continue;
            }
            uint64_t va = page_va[n];
            if (vm_evict(owner, va, leaf)) {
                reclaim_hint = (n + 1) % pool_pages;
                return true;
            }
            /* A full or failing disk cannot be repaired by trying every resident page. */
            return false;
        }
    return false;
}
static bool vm_resolve(unsigned owner, uint64_t va, bool write) {
    va &= ~4095ull;
    uint64_t *leaf = user_leaf(owner, va, false);
    if (!leaf || (*leaf & (VM_ANON | VM_COMMIT)) != (VM_ANON | VM_COMMIT) ||
        (write && !(*leaf & PTE_WRITE)))
        return false;
    if (*leaf & PTE_PRESENT)
        return true;
    uint64_t old = *leaf;
    unsigned swap = (unsigned)((old & PAGE_MASK) / 4096) - 1;
    if ((old & VM_SWAP) &&
        (swap >= VM_SLOTS || swap_owner[swap] != owner + 1 || swap_va[swap] != va))
        return false;
    uint64_t page = alloc_page(owner);
    if (!page)
        return false;
    if (old & VM_SWAP) {
        if (!blob_swap_io || !blob_swap_io(swap, (void *)(uintptr_t)page, false) ||
            vm_hash((void *)(uintptr_t)page) != swap_hash[swap]) {
            unsigned n = (unsigned)((page - POOL_BASE) / 4096);
            page_owner[n] = 0;
            free_pages++;
            return false;
        }
        swap_owner[swap] = 0;
        swap_va[swap] = swap_hash[swap] = 0;
        vm_usage[owner].swapped -= 4096;
        vm_usage[owner].ins++;
    }
    *leaf = page | (old & ~(PAGE_MASK | VM_SWAP)) | PTE_PRESENT;
    page_va[(page - POOL_BASE) / 4096] = (uint32_t)va;
    vm_usage[owner].resident += 4096;
    vm_usage[owner].faults++;
    tasks[owner].info.mapped_bytes += 4096;
    vm_invalidate(owner, va);
    return true;
}
static VMRegion *vm_region(unsigned owner, uint64_t at, uint64_t bytes) {
    if (!bytes || at < VM_FIRST || at >= VM_END || bytes > VM_END - at)
        return 0;
    for (unsigned i = 0; i < VM_REGIONS; i++) {
        VMRegion *r = &vm_regions[owner][i];
        if (r->bytes && at >= r->base && at - r->base <= r->bytes &&
            bytes <= r->bytes - (at - r->base))
            return r;
    }
    return 0;
}
static int64_t vm_reserve(unsigned owner, uint64_t bytes) {
    if (!bytes || bytes > VM_END - VM_FIRST)
        return -ERR_INVAL;
    bytes = (bytes + 4095) & ~4095ull;
    uint64_t limit = tasks[owner].info.capabilities & PROCESS_CAP_SYSTEM ? 512ull * 1024 * 1024
                                                                         : 64ull * 1024 * 1024;
    uint64_t allocated =
        tasks[owner].info.mapped_bytes + vm_usage[owner].reserved - vm_usage[owner].resident;
    if (allocated > limit || bytes > limit - allocated)
        return -ERR_NOMEM;
    VMRegion *available = 0;
    for (unsigned i = 0; i < VM_REGIONS; i++)
        if (!vm_regions[owner][i].bytes) {
            available = &vm_regions[owner][i];
            break;
        }
    if (!available)
        return -ERR_NOMEM;
    uint64_t at = VM_FIRST;
    bool again = true;
    while (again) {
        again = false;
        for (unsigned i = 0; i < VM_REGIONS; i++) {
            VMRegion *r = &vm_regions[owner][i];
            if (r->bytes && at < r->base + r->bytes && r->base < at + bytes) {
                at = r->base + r->bytes;
                again = true;
            }
        }
        if (at > VM_END - bytes)
            return -ERR_NOMEM;
    }
    uint64_t end = at;
    for (; end < at + bytes; end += 4096) {
        uint64_t *leaf = user_leaf(owner, end, true);
        if (!leaf || *leaf)
            break;
        *leaf = VM_ANON | PTE_USER | PTE_WRITE | PTE_NX;
    }
    if (end != at + bytes) {
        for (uint64_t va = at; va < end; va += 4096)
            *user_leaf(owner, va, false) = 0;
        return -ERR_NOMEM;
    }
    *available = (VMRegion){at, bytes};
    vm_usage[owner].reserved += bytes;
    return (int64_t)at;
}
static int64_t vm_operation(unsigned owner, ArkVMRequest *r) {
    if (r->op == ARK_VM_QUERY) {
        r->reserved_bytes = vm_usage[owner].reserved;
        r->committed_bytes = vm_usage[owner].committed;
        r->resident_bytes = vm_usage[owner].resident;
        r->swapped_bytes = vm_usage[owner].swapped;
        r->faults = vm_usage[owner].faults;
        r->pages_in = vm_usage[owner].ins;
        r->pages_out = vm_usage[owner].outs;
        r->pool_bytes = (uint64_t)pool_pages * 4096;
        r->free_bytes = (uint64_t)free_pages * 4096;
        r->swap_capacity_bytes = blob_swap_capacity ? blob_swap_capacity() : 0;
        return 0;
    }
    if (r->op == ARK_VM_RESERVE) {
        int64_t at = vm_reserve(owner, r->bytes);
        if (at < 0)
            return at;
        r->address = (uint64_t)at;
        r->bytes = (r->bytes + 4095) & ~4095ull;
        return 0;
    }
    if ((r->address | r->bytes) & 4095)
        return -ERR_INVAL;
    VMRegion *region = vm_region(owner, r->address, r->bytes);
    if (!region)
        return -ERR_INVAL;
    if (r->op == ARK_VM_RELEASE && (region->base != r->address || region->bytes != r->bytes))
        return -ERR_INVAL;
    if (r->op == ARK_VM_PROTECT || r->op == ARK_VM_COMMIT) {
        if (!(r->flags & ARK_VM_READ) || r->flags & ~(ARK_VM_READ | ARK_VM_WRITE))
            return -ERR_INVAL;
    }
    if (r->op < ARK_VM_COMMIT || r->op > ARK_VM_TRIM)
        return -ERR_INVAL;
    if (vm_remote_active(owner)) {
        if (r->op != ARK_VM_COMMIT)
            return -16;
        /* Committing new nonpresent pages needs no TLB shootdown. Altering a
         * committed page's access permission must wait for sibling CPUs to leave. */
        for (uint64_t va = r->address; va < r->address + r->bytes; va += 4096) {
            uint64_t *leaf = user_leaf(owner, va, false);
            if ((*leaf & VM_COMMIT) &&
                (((*leaf & PTE_WRITE) != 0) != ((r->flags & ARK_VM_WRITE) != 0)))
                return -16;
        }
    }
    uint64_t evicted = 0;
    for (uint64_t va = r->address; va < r->address + r->bytes; va += 4096) {
        uint64_t *leaf = user_leaf(owner, va, false);
        if (r->op == ARK_VM_COMMIT || r->op == ARK_VM_PROTECT) {
            if (!(*leaf & VM_COMMIT) && r->op == ARK_VM_PROTECT)
                continue;
            if (!(*leaf & VM_COMMIT)) {
                *leaf |= VM_COMMIT;
                vm_usage[owner].committed += 4096;
            }
            if (r->flags & ARK_VM_WRITE)
                *leaf |= PTE_WRITE;
            else
                *leaf &= ~PTE_WRITE;
            vm_invalidate(owner, va);
        } else if (r->op == ARK_VM_TRIM) {
            if (*leaf & PTE_PRESENT) {
                if (!vm_evict(owner, va, leaf))
                    return -5;
                evicted += 4096;
            }
        } else {
            vm_free_leaf(owner, va, leaf);
            if (r->op == ARK_VM_RELEASE)
                *leaf = 0;
        }
    }
    if (r->op == ARK_VM_RELEASE) {
        vm_usage[owner].reserved -= region->bytes;
        *region = (VMRegion){0};
    }
    if (r->op == ARK_VM_TRIM)
        r->resident_bytes = evicted;
    return 0;
}
static int64_t vm_call(uint64_t pointer, uint64_t bytes) {
    ArkVMRequest r;
    if (bytes != sizeof r)
        return -ERR_INVAL;
    if (!process_copy_from_user(&r, pointer, sizeof r) ||
        !process_user_range(pointer, sizeof r, true))
        return -ERR_FAULT;
    if ((r.op == ARK_VM_RELEASE || r.op == ARK_VM_DECOMMIT) && r.address < pointer + sizeof r &&
        pointer < r.address + r.bytes)
        return -ERR_INVAL;
    int64_t result = vm_operation(tasks[current].owner, &r);
    if (!process_copy_to_user(pointer, &r, sizeof r))
        return -ERR_FAULT;
    return result;
}

static void vm_performance(ArkPerformanceInfo *info) {
    for (unsigned i = 0; i < PROCESS_MAX; i++)
        if (tasks[i].owner == i && tasks[i].info.state && tasks[i].info.state != PROCESS_DEAD) {
            info->reserved_bytes += vm_usage[i].reserved;
            info->committed_bytes += vm_usage[i].committed;
            info->swapped_bytes += vm_usage[i].swapped;
            info->faults += vm_usage[i].faults;
            info->pages_in += vm_usage[i].ins;
            info->pages_out += vm_usage[i].outs;
        }
    info->swap_capacity_bytes = blob_swap_capacity ? blob_swap_capacity() : 0;
}
