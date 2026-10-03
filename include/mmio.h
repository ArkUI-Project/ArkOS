#ifndef ARK_MMIO_H
#define ARK_MMIO_H
#include "ark.h"
/* Identity-map a validated PCI MMIO range (at most 16 MiB per call).
 * Supports lower-canonical physical addresses, including BARs above 4 GiB.
 * Uses supervisor-only, writable, uncached 2 MiB pages from a fixed pool.
 * Does not allocate RAM, relocate BARs, or relax device-capability bounds.
 */
bool platform_map_mmio(uint64_t physical, uint64_t bytes);
#endif
