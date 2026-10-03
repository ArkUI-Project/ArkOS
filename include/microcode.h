#ifndef ARK_MICROCODE_H
#define ARK_MICROCODE_H
#include "ark.h"
/* Returns size of a structurally valid primary-signature Intel update, or 0.
 * Checksum is corruption detection, not vendor authentication. */
size_t intel_microcode_match(const void *, size_t, uint32_t, uint32_t, uint32_t);
void microcode_init(const void *, size_t);
bool microcode_apply_ap(void);
#endif
