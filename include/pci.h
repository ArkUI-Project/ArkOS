#ifndef ARK_PCI_H
#define ARK_PCI_H
#include "ark.h"
/* One PCI enumeration for the whole kernel. Configuration space access is the
 * only entry point drivers use; every driver previously carried its own copy
 * of the same 0xcf8/0xcfc sequence. Firmware keeps BAR assignment: nothing here
 * relocates, probes or writes a BAR. */
#define ARK_PCI_MAX 64u
typedef struct {
    uint32_t bdf, vendor, device, class_code, subclass, revision, header_type;
    uint32_t subsystem_vendor, subsystem_device, irq_pin, irq_line;
} PciDevice;

/* Bounded single scan. Safe to call twice; the second call is a no-op. */
void pci_init(void);
unsigned pci_count(void);
const PciDevice *pci_device(unsigned index);

uint32_t pci_read(uint32_t bdf, unsigned offset);
uint8_t pci_read8(uint32_t bdf, unsigned offset);
void pci_write16(uint32_t bdf, unsigned offset, uint16_t value);
void pci_write32(uint32_t bdf, unsigned offset, uint32_t value);
/* OR bits into the command register: memory space, bus mastering, INTx disable. */
void pci_command(uint32_t bdf, uint16_t bits);

/* True when this BAR index is the upper half of a 64-bit pair. */
bool pci_bar_is_high(uint32_t bdf, unsigned bar);
/* Decodes a firmware-assigned memory BAR. Rejects I/O, reserved and the upper
 * half of a 64-bit pair. Returns false for an unassigned BAR. */
bool pci_bar_base(uint32_t bdf, unsigned bar, uint64_t *base);
/* First capability with this id, walked with cycle and range guards. */
bool pci_capability(uint32_t bdf, unsigned id, unsigned *offset);
/* Scan for one vendor:device pair. Returns false when the hardware is absent. */
bool pci_find(uint16_t vendor, uint16_t device, uint32_t *bdf);
const char *pci_error(void);

#ifdef ARK_PCI_HOST_TEST
/* Host tests supply a synthetic configuration space. */
void pci_test_set_config(uint32_t (*read)(uint32_t, unsigned));
#endif
#endif