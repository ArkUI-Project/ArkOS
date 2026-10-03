#ifndef ARK_VIRTIO_GPU_H
#define ARK_VIRTIO_GPU_H
#include "ark_api.h"
/* Probe/maps PCI before per-process page tables are installed. Resource
 * allocation and shader verification are lazy, after the physical pool exists. */
bool virtio_gpu_init(unsigned width, unsigned height);
bool virtio_gpu_ready(void);
int64_t virtio_gpu_glass(ArkGlassRequest *request);
#endif
