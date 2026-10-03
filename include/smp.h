#ifndef ARK_SMP_H
#define ARK_SMP_H
#include "ark.h"
void smp_init(void);
unsigned platform_online_cpus(void);
void platform_smp_eoi(void);
void smp_copy(void *, const void *, size_t);
#endif
