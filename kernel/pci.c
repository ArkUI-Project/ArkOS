/* Shared PCI configuration space and enumeration. Drivers keep their own DMA
 * rings and transports; this file only decodes what firmware already assigned. */
#include "pci.h"
#include "device.h"

static PciDevice table[ARK_PCI_MAX];
static unsigned table_count;
static bool scanned;
static char error_text[64] = "";

/* Standard PCI base class names. Unknown classes stay numeric: no model string
 * is invented for hardware the kernel cannot name. */
static void hex32(char *out, size_t cap, uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    if (!cap)
        return;
    unsigned width = cap - 1 < 8 ? cap - 1 : 8;
    for (unsigned i = 0; i < width; i++)
        out[i] = digits[(value >> ((width - 1 - i) * 4)) & 0xf];
    out[width] = 0;
}
static void class_name(char *out, size_t cap, uint32_t class_code, uint32_t subclass) {
    static const char *const names[] = {
        "Unclassified",    "Mass storage", "Network",      "Display",
        "Multimedia",      "Memory",       "Bridge",       "Communication",
        "System",          "Input",        "Docking",      "Processor",
        "Serial bus",      "Wireless",     "Intelligent",  "Satellite",
        "Encryption",      "Signal",       "Processing acceleration", "Non-VGA unclassified"};
    const char *name =
        class_code < sizeof names / sizeof names[0] ? names[class_code] : "Unknown class";
    strcopy(out, name, cap);
    char hex[8];
    hex32(hex, sizeof hex, ((class_code << 8) | subclass) & 0xffffu);
    size_t at = strlen(out);
    if (at + 2 < cap) {
        out[at] = ' ';
        strcopy(out + at + 1, hex, cap - at - 1);
    }
}

static inline void out32(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0,%1" ::"a"(value), "Nd"(port));
}
static inline uint32_t in32(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1,%0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void out16(uint16_t port, uint16_t value) {
    __asm__ volatile("outw %0,%1" ::"a"(value), "Nd"(port));
}
#ifdef ARK_PCI_HOST_TEST
static uint32_t (*test_reader)(uint32_t, unsigned);
void pci_test_set_config(uint32_t (*read)(uint32_t, unsigned)) {
    test_reader = read;
    scanned = false;
    table_count = 0;
}
#else
static inline uint32_t port_read(uint32_t bdf, unsigned offset) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    return in32(0xcfc);
}
#endif
uint32_t pci_read(uint32_t bdf, unsigned offset) {
#ifdef ARK_PCI_HOST_TEST
    return test_reader ? test_reader(bdf, offset) : 0xffffffffu;
#else
    return port_read(bdf, offset);
#endif
}
uint8_t pci_read8(uint32_t bdf, unsigned offset) {
    return (uint8_t)(pci_read(bdf, offset) >> ((offset & 3) * 8));
}
void pci_write16(uint32_t bdf, unsigned offset, uint16_t value) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    out16((uint16_t)(0xcfc + (offset & 2)), value);
}
void pci_write32(uint32_t bdf, unsigned offset, uint32_t value) {
    out32(0xcf8, 0x80000000u | bdf | (offset & 0xfcu));
    out32(0xcfc, value);
}
void pci_command(uint32_t bdf, uint16_t bits) {
    pci_write16(bdf, 4, (uint16_t)(pci_read(bdf, 4) | bits));
}
const char *pci_error(void) {
    return error_text;
}
bool pci_bar_is_high(uint32_t bdf, unsigned bar) {
    if (!bar)
        return false;
    uint32_t previous = pci_read(bdf, 0x10 + (bar - 1) * 4);
    return !(previous & 1) && (previous & 6) == 4;
}
bool pci_bar_base(uint32_t bdf, unsigned bar, uint64_t *base) {
    if (bar > 5 || pci_bar_is_high(bdf, bar))
        return false;
    uint32_t low = pci_read(bdf, 0x10 + bar * 4);
    if ((low & 1) || (low & 6) == 2 || (low & 6) == 6)
        return false;
    uint64_t at = low & ~15u;
    if ((low & 6) == 4)
        at |= (uint64_t)pci_read(bdf, 0x14 + bar * 4) << 32;
    if (!at)
        return false;
    if (base)
        *base = at;
    return true;
}
bool pci_capability(uint32_t bdf, unsigned id, unsigned *offset) {
    uint8_t visited[256] = {0};
    unsigned cap = pci_read8(bdf, 0x34) & 0xfcu;
    for (unsigned count = 0; cap && count < 48; count++) {
        if (cap < 0x40 || cap > 0xfc || visited[cap])
            return false;
        visited[cap] = 1;
        if (pci_read8(bdf, cap) == id) {
            if (offset)
                *offset = cap;
            return true;
        }
        cap = pci_read8(bdf, cap + 1) & 0xfcu;
    }
    return false;
}
void pci_init(void) {
    if (scanned)
        return;
    scanned = true;
    table_count = 0;
    error_text[0] = 0;
    for (unsigned bus = 0; bus < 256 && table_count < ARK_PCI_MAX; bus++)
        for (unsigned dev = 0; dev < 32 && table_count < ARK_PCI_MAX; dev++) {
            uint32_t bdf = (bus << 16) | (dev << 11), id = pci_read(bdf, 0);
            if ((id & 0xffff) == 0xffff || !(id & 0xffff))
                continue;
            unsigned functions = (pci_read(bdf, 0x0c) & 0x800000) ? 8 : 1;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t entry = bdf | (function << 8), vendor = pci_read(entry, 0);
                uint16_t v = (uint16_t)vendor;
                if (v == 0xffff || !v)
                    break;
                uint32_t class_code = pci_read(entry, 0x08);
                PciDevice *d = &table[table_count];
                d->bdf = entry;
                d->vendor = v;
                d->device = vendor >> 16;
                /* Offset 0x08 packs revision, programming interface, subclass
                 * and base class, one byte each from the low end. */
                d->class_code = class_code >> 24;
                d->subclass = (class_code >> 16) & 0xff;
                d->revision = class_code & 0xff;
                d->header_type = pci_read8(entry, 0x0e);
                d->subsystem_vendor = pci_read(entry, 0x2c) & 0xffff;
                d->subsystem_device = pci_read(entry, 0x2c) >> 16;
                d->irq_pin = pci_read8(entry, 0x3d);
                d->irq_line = pci_read8(entry, 0x3c);
                table_count++;
                /* Every enumerated function becomes a real inventory node, so
                 * hardware no driver claimed stays visible with its actual
                 * vendor, device and PCI class instead of disappearing. */
                ArkDeviceInfo info = {0};
                info.class_id = ARK_DEV_CLASS_PCI;
                info.bus = ARK_BUS_PCI;
                info.bdf = entry;
                info.vendor = d->vendor;
                info.device = d->device;
                info.device_class = (d->class_code << 8) | d->subclass;
                info.flags = ARK_DEV_PRESENT;
                info.state = ARK_DEV_STATE_OK;
                info.unit = (bus << 8) | (dev << 3) | function;
                class_name(info.name, sizeof info.name, d->class_code, d->subclass);
                hex32(info.detail, sizeof info.detail, d->vendor);
                strcopy(info.detail + 8, " ", sizeof info.detail - 8);
                hex32(info.detail + 9, sizeof info.detail - 9, d->device);
                strcopy(info.driver, "pci", sizeof info.driver);
                device_register(&info);
            }
        }
}
unsigned pci_count(void) {
    return table_count;
}
const PciDevice *pci_device(unsigned index) {
    return index < table_count ? &table[index] : 0;
}
bool pci_find(uint16_t vendor, uint16_t device, uint32_t *bdf) {
    for (unsigned bus = 0; bus < 256; bus++)
        for (unsigned dev = 0; dev < 32; dev++) {
            uint32_t at = (bus << 16) | (dev << 11), id = pci_read(at, 0);
            if ((id & 0xffff) == 0xffff || !(id & 0xffff))
                continue;
            unsigned functions = (pci_read(at, 0x0c) & 0x800000) ? 8 : 1;
            for (unsigned function = 0; function < functions; function++) {
                uint32_t entry = at | (function << 8), found = pci_read(entry, 0);
                uint16_t id = (uint16_t)found;
                /* 0xffff and 0x0000 are absent slots, never a device match. */
                if (id == 0xffff || !id)
                    continue;
                if (id == vendor && (found >> 16) == device) {
                    if (bdf)
                        *bdf = entry;
                    return true;
                }
            }
        }
    return false;
}