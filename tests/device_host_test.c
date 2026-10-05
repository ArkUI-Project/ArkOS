/* Kernel device model and PCI decoding against a synthetic bus. No host
 * hardware and no fabricated inventory: every assertion below is about kernel
 * policy, bounds and lifecycle behavior.
 * gcc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
 *   -DARK_DEVICE_HOST_TEST -DARK_PCI_HOST_TEST -Iinclude \
 *   tests/device_host_test.c kernel/device.c kernel/pci.c kernel/lib.c \
 *   -o build/device-host-test
 * ASAN_OPTIONS=detect_leaks=0 ./build/device-host-test
 */
#define ARK_KERNEL
#include "ark_api.h"
#include "device.h"
#include "pci.h"
#include "block.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures, wakes, block_reads;
static uint32_t last_wake;

#define CHECK(test)                                                                                \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(test)) {                                                                             \
            ++failures;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #test);                        \
        }                                                                                          \
    } while (0)

/* ---- process and block seams ------------------------------------------------ */
void process_wake(uint32_t pid) {
    wakes++;
    last_wake = pid;
}
uint32_t process_current_pid(void) {
    return 7;
}
static BlockDevice disks[2];
BlockDevice *block_device(unsigned id) {
    return id < 2 ? &disks[id] : 0;
}
bool block_read(BlockDevice *d, uint64_t lba, uint32_t sectors, void *buffer) {
    if (!d || !d->present || lba >= d->sectors || sectors > d->sectors - lba)
        return false;
    block_reads++;
    memset(buffer, 0xa5, sectors * 512u);
    return true;
}
bool block_write(BlockDevice *d, uint64_t lba, uint32_t sectors, const void *buffer) {
    (void)buffer;
    return d && d->present && lba < d->sectors && sectors <= d->sectors - lba;
}
bool block_flush(BlockDevice *d) {
    return d && d->present;
}
const char *block_error(void) {
    return "synthetic transport";
}

/* ---- synthetic PCI configuration space ------------------------------------- */
#define BUSES 2
#define DEVS 32
#define FUNCS 8
#define WORDS 64
static uint32_t config[BUSES][DEVS][FUNCS][WORDS];
static uint32_t synthetic_read(uint32_t bdf, unsigned offset) {
    unsigned bus = (bdf >> 16) & 0xff, dev = (bdf >> 11) & 0x1f, function = (bdf >> 8) & 0x7;
    if (bus >= BUSES || dev >= DEVS || function >= FUNCS || offset >= WORDS * 4)
        return 0xffffffffu;
    return config[bus][dev][function][offset / 4];
}
static void put(unsigned bus, unsigned dev, unsigned function, unsigned offset, uint32_t value) {
    config[bus][dev][function][offset / 4] = value;
}
static void install(unsigned bus, unsigned dev, unsigned function, uint32_t vendor,
                    uint32_t device, uint32_t class_code) {
    put(bus, dev, function, 0, (device << 16) | vendor);
    put(bus, dev, function, 4, 0x00000006);      /* memory space, bus mastering */
    put(bus, dev, function, 8, class_code);
    put(bus, dev, function, 0x0c, 0x00800000);   /* status: multi-function device */
}
static void reset_bus(void) {
    memset(config, 0xff, sizeof config);
}

/* ---- tests ------------------------------------------------------------------ */
static void noop_poll(void) {
}
static ArkDeviceInfo node(uint32_t class_id, uint32_t unit, const char *name) {
    ArkDeviceInfo info = {0};
    info.class_id = class_id;
    info.bus = ARK_BUS_ISA;
    info.unit = unit;
    info.flags = ARK_DEV_PRESENT;
    info.state = ARK_DEV_STATE_OK;
    strcopy(info.name, name, sizeof info.name);
    strcopy(info.driver, "test", sizeof info.driver);
    return info;
}
static void registration_policy(void) {
    device_init();
    ArkDeviceInfo first = node(ARK_DEV_CLASS_BLOCK, 0, "Disk port 0");
    first.blocks = 1024;
    CHECK(device_register(&first) == 0);
    CHECK(device_count() == 1);
    CHECK(strcmp(device_info(0)->name, "Disk port 0") == 0);
    CHECK(device_info(0)->blocks == 1024);

    /* A re-probe of the same hardware updates in place and keeps counters. */
    ArkDeviceInfo again = first;
    strcopy(again.name, "Disk port 0 (probed)", sizeof again.name);
    again.blocks = 2048;
    CHECK(device_register(&again) == 0);
    CHECK(device_count() == 1);
    CHECK(device_info(0)->blocks == 2048);
    CHECK(strcmp(device_info(0)->name, "Disk port 0 (probed)") == 0);

    /* Malformed descriptors never reach the table. */
    ArkDeviceInfo bad = node(ARK_DEV_CLASS_BLOCK, 1, "unterminated");
    memset(bad.name, 'x', sizeof bad.name);
    CHECK(device_register(&bad) == -22);
    bad = node(ARK_DEV_CLASS_BLOCK, 1, "bad class");
    bad.class_id = 99;
    CHECK(device_register(&bad) == -22);
    bad = node(ARK_DEV_CLASS_BLOCK, 1, "bad flags");
    bad.flags = 0x8000u;
    CHECK(device_register(&bad) == -22);
    bad = node(ARK_DEV_CLASS_BLOCK, 1, "bad state");
    bad.state = 9;
    CHECK(device_register(&bad) == -22);
    CHECK(device_count() == 1);
    CHECK(strstr(device_error(), "invalid") != 0);

    /* The table is bounded and a full table fails only the caller. */
    for (uint32_t unit = 1; unit < ARK_DEVICE_MAX; unit++)
        CHECK(device_register(&(ArkDeviceInfo){.class_id = ARK_DEV_CLASS_SENSOR,
                                               .unit = unit,
                                               .name = "Sensor",
                                               .driver = "test"}) >= 0);
    CHECK(device_count() == ARK_DEVICE_MAX);
    ArkDeviceInfo overflow = node(ARK_DEV_CLASS_SENSOR, ARK_DEVICE_MAX, "Overflow");
    CHECK(device_register(&overflow) == -12);
    CHECK(device_count() == ARK_DEVICE_MAX);
}
static void state_and_counters(void) {
    device_init();
    ArkDeviceInfo link = node(ARK_DEV_CLASS_NETWORK, 0, "Ethernet");
    uint32_t index = (uint32_t)device_register(&link);
    CHECK(index == 0);
    CHECK(device_set_state(index, ARK_DEV_PRESENT, ARK_DEV_STATE_ERROR));
    CHECK(device_info(index)->state == ARK_DEV_STATE_ERROR);
    CHECK(device_info(index)->generation == 1);
    CHECK(!device_set_state(index, ARK_DEV_PRESENT, 7));
    CHECK(!device_set_state(index, 0x8000u, ARK_DEV_STATE_OK));
    CHECK(!device_set_state(ARK_DEVICE_MAX, ARK_DEV_PRESENT, ARK_DEV_STATE_OK));
    /* An unchanged state is not a new event. */
    CHECK(device_set_state(index, ARK_DEV_PRESENT, ARK_DEV_STATE_ERROR));
    CHECK(device_info(index)->generation == 1);
    CHECK(device_add_counters(index, 100, 40, 3, 1));
    CHECK(device_add_counters(index, 1, 2, 1, 0));
    CHECK(device_info(index)->rx_bytes == 101 && device_info(index)->tx_bytes == 42);
    CHECK(device_info(index)->ops == 4 && device_info(index)->errors == 1);
    CHECK(!device_add_counters(ARK_DEVICE_MAX, 1, 1, 1, 1));
    unsigned polled = 0;
    CHECK(!device_register_poll(0));
    while (device_register_poll(noop_poll))
        polled++;
    CHECK(polled == ARK_DEVICE_POLL_MAX);
    CHECK(!device_register_poll(noop_poll));
}
static void event_bridge(void) {
    device_init();
    ArkDeviceInfo link = node(ARK_DEV_CLASS_NETWORK, 0, "Ethernet");
    uint32_t index = (uint32_t)device_register(&link);
    uint32_t generation = device_generation();
    CHECK(!device_pending(7));            /* never asked for inventory */
    wakes = last_wake = 0;
    device_note(7);
    CHECK(!device_pending(7));
    CHECK(device_notify(index));
    CHECK(device_generation() != generation);
    CHECK(device_pending(7));
    CHECK(wakes == 1 && last_wake == 7);
    device_note(7);
    CHECK(!device_pending(7));
    wakes = 0;
    device_note(9);
    wakes = 0;
    device_notify(index);
    CHECK(wakes == 2 && last_wake == 9); /* both noted processes are woken */
    device_forget_pid(9);
    CHECK(!device_pending(9));
    device_session_reset();
    CHECK(!device_pending(7));
    CHECK(!device_notify(ARK_DEVICE_MAX));
    CHECK(device_info(0)->generation == 2);
}
static void read_budget(void) {
    device_init();
    for (unsigned i = 0; i < ARK_DEVICE_READ_BURST; i++)
        CHECK(device_read_allowed(7, 1000));
    CHECK(!device_read_allowed(7, 1000));
    CHECK(device_read_allowed(7, 1000 + ARK_DEVICE_READ_WINDOW_MS));
    CHECK(device_read_allowed(8, 1000));  /* per process budget */
    device_session_reset();
    CHECK(device_read_allowed(7, 1000));
}
static void block_policy(void) {
    device_init();
    memset(disks, 0, sizeof disks);
    disks[0].present = true;
    disks[0].sectors = 64;
    disks[1].present = true;
    disks[1].sectors = 32;
    ArkDeviceInfo system = node(ARK_DEV_CLASS_BLOCK, 0, "Disk port 0");
    system.blocks = 64;
    system.flags |= ARK_DEV_SYSTEM_VOLUME;
    CHECK(device_register(&system) == 0);
    ArkDeviceInfo external = node(ARK_DEV_CLASS_BLOCK, 1, "Disk port 1");
    external.blocks = 32;
    external.flags &= (uint32_t)~ARK_DEV_READABLE;
    CHECK(device_register(&external) == 1);
    static uint8_t buffer[2048];
    CHECK(!device_block_read(0, 0, 1, buffer));     /* system volume */
    CHECK(!device_block_read(1, 0, 1, buffer));     /* not readable */
    CHECK(!device_block_read(2, 0, 1, buffer));     /* no such node */
    external.flags |= ARK_DEV_READABLE | ARK_DEV_WRITABLE;
    CHECK(device_register(&external) == 1);
    CHECK(device_info(1)->flags & ARK_DEV_READABLE);
    CHECK(device_block_read(1, 0, 4, buffer));
    CHECK(block_reads == 1);
    CHECK(!device_block_read(1, 30, 8, buffer));    /* past the end */
    CHECK(!device_block_read(1, 0, 0, buffer));     /* empty request */
    CHECK(!device_block_read(1, 0, 1, 0));          /* no destination */
    CHECK(block_reads == 1);
    CHECK(device_block_flush(1));
    CHECK(!device_block_flush(0));                  /* system volume is not flushed here */
    CHECK(device_block_flush(ARK_DEVICE_MAX) == false);
}
static void pci_enumeration(void) {
    reset_bus();
    device_init();
    pci_test_set_config(synthetic_read);
    pci_init();
    CHECK(pci_count() == 0);
    install(0, 0, 0, 0x8086, 0x1237, 0x06000000);  /* PCI bridge */
    install(0, 1, 0, 0x8086, 0x2922, 0x01060100);  /* SATA AHCI */
    install(0, 1, 1, 0x8086, 0x2923, 0x01060100);  /* second function */
    put(0, 1, 0, 0x10, 0xfebf1000 | 4);           /* 64-bit BAR */
    put(0, 1, 0, 0x14, 0x00000000);
    put(0, 1, 0, 0x18, 0xfebf2000 | 4);
    put(0, 1, 0, 0x1c, 0x00000000);
    put(0, 1, 0, 0x34, 0x40);                     /* capability list */
    put(0, 1, 0, 0x40, 0x01000009 | (0x50 << 8));  /* MSI-X length 0x50, next 0x50 */
    put(0, 1, 0, 0x90, 0x00000009 | 0x00);        /* cycle back to 0x40 */
    pci_test_set_config(synthetic_read);
    pci_init();
    CHECK(pci_count() == 3);
    const PciDevice *ahci = 0;
    for (unsigned i = 0; i < pci_count(); i++)
        if (pci_device(i)->device == 0x2922)
            ahci = pci_device(i);
    CHECK(ahci != 0);
    if (ahci) {
        CHECK(ahci->vendor == 0x8086 && ahci->class_code == 0x01 && ahci->subclass == 0x06);
        CHECK(((ahci->bdf >> 11) & 0x1f) == 1 && (ahci->bdf & 0xff) == 0);
    }
    /* Every enumerated function becomes an inventory node. */
    CHECK(device_count() == 3);
    CHECK(strcmp(device_info(0)->driver, "pci") == 0);
    CHECK(strncmp(device_info(1)->name, "Mass storage", 12) == 0);
    CHECK(device_info(1)->class_id == ARK_DEV_CLASS_PCI);
    CHECK(device_info(1)->vendor == 0x8086 && device_info(1)->device == 0x2922);

    /* BAR decoding keeps firmware assignment and rejects unusable shapes.
     * BAR0 and BAR2 are 64-bit pairs, so BAR1 and BAR3 are their high halves. */
    uint64_t base = 0;
    CHECK(pci_bar_base(ahci->bdf, 0, &base) && base == 0xfebf1000ull);
    CHECK(pci_bar_base(ahci->bdf, 2, &base) && base == 0xfebf2000ull);
    CHECK(pci_bar_is_high(ahci->bdf, 1) && pci_bar_is_high(ahci->bdf, 3));
    CHECK(!pci_bar_is_high(ahci->bdf, 0) && !pci_bar_is_high(ahci->bdf, 2));
    CHECK(!pci_bar_base(ahci->bdf, 1, &base));   /* upper half of a 64-bit pair */
    CHECK(!pci_bar_base(ahci->bdf, 3, &base));
    CHECK(!pci_bar_base(ahci->bdf, 6, &base));   /* beyond BAR5 */
    put(0, 1, 0, 0x20, 0x00000001);              /* BAR4 is an I/O range */
    CHECK(!pci_bar_base(ahci->bdf, 4, &base));
    put(0, 1, 0, 0x20, 0x00000006);              /* reserved memory type */
    CHECK(!pci_bar_base(ahci->bdf, 4, &base));
    put(0, 1, 0, 0x20, 0x00000000);              /* firmware left it unassigned */
    CHECK(!pci_bar_base(ahci->bdf, 4, &base));

    /* A declared capability is found through the bounded walk. */
    unsigned offset = 0;
    put(0, 1, 0, 0x34, 0x40);
    put(0, 1, 0, 0x40, 0x00100009u | (0x50u << 8));  /* MSI-X, next 0x50 */
    put(0, 1, 0, 0x50, 0x00030001u);                 /* power management, end */
    CHECK(pci_capability(ahci->bdf, 9, &offset) && offset == 0x40);
    CHECK(pci_capability(ahci->bdf, 1, &offset) && offset == 0x50);
    CHECK(!pci_capability(ahci->bdf, 4, &offset));
    /* A capability list that points back at itself must terminate. */
    put(0, 1, 0, 0x34, 0x40);
    put(0, 1, 0, 0x40, 0x00030042u | (0x40u << 8));
    CHECK(!pci_capability(ahci->bdf, 0x99, &offset));
    /* An out-of-range next pointer must terminate too. */
    put(0, 1, 0, 0x40, 0x00030042u | (0xfeu << 8));
    CHECK(!pci_capability(ahci->bdf, 0x99, &offset));
    CHECK(pci_read8(ahci->bdf, 0x34) == 0x40);
    CHECK((pci_read(ahci->bdf, 0) & 0xffff) == 0x8086);

    uint32_t bdf = 0;
    CHECK(pci_find(0x8086, 0x2922, &bdf) && bdf == ahci->bdf);
    CHECK(pci_find(0x8086, 0x2923, &bdf) && bdf == (ahci->bdf | 0x100));
    CHECK(!pci_find(0xdead, 0xbeef, &bdf));
    CHECK(!pci_find(0xffff, 0xffff, &bdf)); /* empty slot is not a match */
    CHECK(!pci_find(0x0000, 0x0000, &bdf));
    CHECK(pci_count() == 3);
    pci_init(); /* a second scan is a no-op */
    CHECK(pci_count() == 3);
}
int main(void) {
    registration_policy();
    state_and_counters();
    event_bridge();
    read_budget();
    block_policy();
    pci_enumeration();
    printf("Device model: %u checks, %u failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}