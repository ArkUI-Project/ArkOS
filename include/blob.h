#ifndef ARK_BLOB_H
#define ARK_BLOB_H
#define ARK_KERNEL
#include "ark_api.h"
/* Internal only: buffer is a bounded kernel allocation, UID comes from session.
 * @pkg. objects are invisible and inaccessible through the public BLOB syscall. */
int64_t blob_kernel_request(ArkBlobRequest *request, uint32_t uid);
/* Page backing is allocated through the COW extent allocator. It never uses
 * an unclaimed raw disk range and is hidden from every userspace namespace. */
bool blob_swap_io(unsigned slot, void *page, bool write);
uint64_t blob_swap_capacity(void);
#ifdef ARK_BLOB_HOST_TEST
void blob_test_reset(void);
#endif
#endif
