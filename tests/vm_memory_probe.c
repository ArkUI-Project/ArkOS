/* Guest probe: CPU page faults and real ArkFS/block swap, not a host model. */
#include "ark_api.h"
#include "ark.h"
static void log(const char *s) {
    ark_syscall6(ARK_SYS_LOG, (uintptr_t)s, strlen(s), 0, 0, 0, 0);
}
static void require(bool yes, unsigned n) {
    if (!yes) {
        char b[24];
        log("[vm-probe] FAIL ");
        uint_to_str(n, b);
        log(b);
        log("\n");
        ark_exit(1);
    }
}
static ArkVMRequest query(void) {
    ArkVMRequest q = {.op = ARK_VM_QUERY};
    require(ark_vm(&q) == 0, 1);
    return q;
}
static ArkVMRequest reserve(uint64_t bytes) {
    ArkVMRequest q = {.op = ARK_VM_RESERVE, .bytes = bytes};
    require(ark_vm(&q) == 0, 2);
    return q;
}
static void commit(ArkVMRequest *q) {
    q->op = ARK_VM_COMMIT;
    q->flags = ARK_VM_READ | ARK_VM_WRITE;
    require(ark_vm(q) == 0, 3);
}
static uint64_t pattern(unsigned i) {
    return 0xd1a54b3290000000ull ^ ((uint64_t)i * 0x9e3779b97f4a7c15ull);
}
#ifndef VM_WRITE_FAILURE
static void wait_child(unsigned stage, int status) {
    int64_t pid = ark_syscall6(900, stage, 0, 0, 0, 0, 0);
    require(pid > 0, 40 + stage);
    for (unsigned i = 0; i < 1000; i++) {
        int64_t s = ark_syscall6(901, (uint64_t)pid, 0, 0, 0, 0, 0);
        if (s != INT64_MIN) {
            require(s == status, 50 + stage);
            return;
        }
        ark_wait_ms(10);
    }
    require(false, 60 + stage);
}
#endif
int main(void) {
    int64_t stage = ark_syscall6(903, 0, 0, 0, 0, 0, 0);
    if (stage > 0) {
        ArkVMRequest r = reserve(4096);
        if (stage != 2)
            commit(&r);
        volatile uint64_t *p = (void *)(uintptr_t)r.address;
        if (stage == 1) {
            r.op = ARK_VM_PROTECT;
            r.flags = ARK_VM_READ;
            require(ark_vm(&r) == 0, 80);
            *p = 1;
        }
        if (stage == 2) {
            *p = 2;
        }
        if (stage == 3) {
            *p = 0xc3;
            ((void (*)(void))(uintptr_t)r.address)();
        }
        require(false, 81);
        return 1;
    }
    ArkVMRequest before = query(), r = reserve(16 * 4096), after = query();
    require(after.resident_bytes == before.resident_bytes, 4);
    commit(&r);
    after = query();
    require(after.resident_bytes == before.resident_bytes &&
                after.committed_bytes == before.committed_bytes + r.bytes,
            5);
    volatile uint64_t *p = (void *)(uintptr_t)r.address;
    for (unsigned i = 0; i < 16; i++) {
        require(p[i * 512] == 0 && p[i * 512 + 511] == 0, 6);
        p[i * 512] = pattern(i);
        p[i * 512 + 511] = ~pattern(i);
    }
    after = query();
    require(after.resident_bytes == before.resident_bytes + r.bytes, 7);
    r.op = ARK_VM_TRIM;
#ifdef VM_WRITE_FAILURE
    require(ark_vm(&r) < 0, 8);
    after = query();
    require(after.resident_bytes == before.resident_bytes + r.bytes &&
                after.swapped_bytes == before.swapped_bytes,
            9);
    for (unsigned i = 0; i < 16; i++)
        require(p[i * 512] == pattern(i) && p[i * 512 + 511] == ~pattern(i), 10);
    r.op = ARK_VM_RELEASE;
    require(ark_vm(&r) == 0, 11);
    require(ark_syscall6(905, 0, 0, 0, 0, 0, 0) == 1, 12);
    log("[vm-probe] Denied disk writes preserve resident pages and files PASS\n");
    return 0;
#else
    require(ark_vm(&r) == 0, 8);
    after = query();
    require(after.swapped_bytes == before.swapped_bytes + 16 * 4096 &&
                after.resident_bytes == before.resident_bytes,
            9);
    for (unsigned i = 0; i < 16; i++)
        require(p[i * 512] == pattern(i) && p[i * 512 + 511] == ~pattern(i), 10);
    after = query();
    require(after.swapped_bytes == before.swapped_bytes && after.pages_in >= before.pages_in + 16,
            11);
    r.op = ARK_VM_RELEASE;
    require(ark_vm(&r) == 0, 12);
    ArkVMRequest again = reserve(4096);
    require(again.address == r.address, 13);
    commit(&again);
    require(*(volatile uint64_t *)(uintptr_t)again.address == 0, 14);
    again.op = ARK_VM_DECOMMIT;
    require(ark_vm(&again) == 0, 15);
    commit(&again);
    require(*(volatile uint64_t *)(uintptr_t)again.address == 0, 16);
    again.op = ARK_VM_RELEASE;
    require(ark_vm(&again) == 0, 17);
    ArkVMRequest bad = {.op = ARK_VM_RELEASE, .address = 0x100000, .bytes = 4096};
    require(ark_vm(&bad) == -22, 18);
    bad = (ArkVMRequest){.op = ARK_VM_RESERVE, .bytes = UINT64_MAX};
    require(ark_vm(&bad) == -22, 19);
    log("[vm-probe] Reserve, demand zero, commit, swap round-trip, decommit, release, reuse "
        "PASS\n");
    wait_child(1, -142);
    wait_child(2, -142);
    wait_child(3, -142);
    log("[vm-probe] Read-only, uncommitted and NX CPU faults isolate children PASS\n");
    before = query();
    r = reserve((before.free_bytes + 2 * 1024 * 1024 + 4095) & ~4095ull);
    commit(&r);
    p = (void *)(uintptr_t)r.address;
    unsigned pages = (unsigned)(r.bytes / 4096);
    for (unsigned i = 0; i < pages; i++)
        p[i * 512] = pattern(i);
    after = query();
    require(after.swapped_bytes > before.swapped_bytes && after.pages_out > before.pages_out, 20);
    for (unsigned i = 0; i < 32; i++)
        require(p[i * 512] == pattern(i), 21);
    for (unsigned i = 512; i < pages; i += 512)
        require(p[i * 512] == pattern(i), 22);
    require(p[(pages - 1) * 512] == pattern(pages - 1), 23);
    r.op = ARK_VM_RELEASE;
    require(ark_vm(&r) == 0, 24);
    require(query().swapped_bytes == 0, 25);
    require(ark_syscall6(905, 0, 0, 0, 0, 0, 0) == 1, 26);
    log("[vm-probe] Real physical exhaustion, automatic page-out/page-in and file preservation "
        "PASS\n");
    return 0;
#endif
}
