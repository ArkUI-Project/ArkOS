/* Example .arco kernel driver: a live counter node.
 *
 * It proves the whole loadable path without claiming hardware that does not
 * exist. On INIT it walks the real PCI bus through the host table, registers
 * one PCI-class node describing what it actually observed, and arms a poll
 * callback that advances the node's operation counters and wakes watchers so
 * the desktop device page repaints on its own. Nothing here is fabricated: the
 * counters come from the driver's own poll, and the PCI count comes from the
 * kernel's decoder.
 *
 * Build: python3 scripts/arco.py sdk/driver_demo.c -o build/demo.arco \
 *            --name demo --version 1.0.0
 * Requires an active admin session: dev install /mnt/fat32/demo.arco
 */
#include "driver.h"

#define DRIVER_NAME "demo"
#define POLL_PERIOD_MS 500u

static const ArkDriverHost *host;
static int node = -1;
static uint64_t last_millis;
static uint32_t pci_seen;
static uint64_t ticks;

static void demo_poll(void) {
    uint64_t now = host->millis();
    if (now - last_millis < POLL_PERIOD_MS)
        return;
    last_millis = now;
    ++ticks;
    /* One completed "operation" per tick, reported to the kernel's device
     * model; the node's stats then move for every watcher. */
    if (node >= 0 && host->device_add_counters((uint32_t)node, 0, 0, 1, 0))
        host->device_notify((uint32_t)node);
}

/* Count what the kernel's PCI decoder can see. The demo reports the real
 * number rather than a fixed value. */
static unsigned count_pci(void) {
    unsigned total = host->pci_count(), matched = 0;
    for (unsigned i = 0; i < total; i++) {
        uint32_t record[10];
        if (!host->pci_device(i, record))
            break;
        if (record[1] && record[2]) /* vendor and device id both decoded */
            ++matched;
    }
    return matched;
}

int64_t arco_entry(const ArkDriverHost *table, uint32_t op) {
    host = table;
    if (op == ARCO_OP_DEINIT) {
        if (node >= 0)
            host->device_set_state((uint32_t)node, 0, ARK_DEV_STATE_ABSENT);
        node = -1;
        host->log("demo deinitialised");
        return 0;
    }
    if (op != ARCO_OP_INIT)
        return -22;

    pci_seen = count_pci();
    last_millis = host->millis();

    ArkDeviceInfo info;
    ark_drv_node(&info, ARK_DEV_CLASS_PCI, 0, "Demo counter",
                 DRIVER_NAME, "loadable module");
    info.blocks = pci_seen;          /* devices observed on the PCI bus */
    info.rx_bytes = 0;
    ark_strcat(info.detail, "; pci devices seen: ", sizeof info.detail);
    char number[16];
    number[0] = '0' + (char)(pci_seen / 10);
    number[1] = '0' + (char)(pci_seen % 10);
    number[2] = 0;
    ark_strcat(info.detail, number, sizeof info.detail);
    node = host->device_register(&info);
    if (node < 0)
        return -12;
    if (host->device_register_poll(demo_poll) < 0)
        return -16;
    host->log("demo module online");
    return 0;
}