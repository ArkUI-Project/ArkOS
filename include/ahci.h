#ifndef ARK_AHCI_H
#define ARK_AHCI_H
#include "ark.h"
unsigned ahci_init(void);
uint64_t ahci_sectors(unsigned);
bool ahci_transfer(unsigned, uint64_t, uint32_t, void *, bool);
bool ahci_flush(unsigned);
bool ahci_cd_read(uint32_t lba, unsigned count, void *buffer);
#endif
