/* Small executable fixture keeps the count test about metadata, not font size. */
#include "ark_api.h"
static uint32_t pixels[128 * 80];
int main(void) {
    ArkSurfaceRequest q = {
        .op = ARK_SURFACE_CREATE, .width = 128, .height = 80, .title = "Package fixture"};
    if (ark_surface(&q) < 0)
        return 1;
    uint32_t id = q.id;
    for (unsigned i = 0; i < 128 * 80; i++)
        pixels[i] = 0x478bc8;
    q = (ArkSurfaceRequest){.op = ARK_SURFACE_PRESENT,
                            .id = id,
                            .width = 128,
                            .height = 80,
                            .stride = 128,
                            .flags = ARK_PRESENT_FULL,
                            .pixels = (uintptr_t)pixels};
    if (ark_surface(&q) < 0)
        return 2;
    static const char message[] = "[package-count] executable surface PASS\n";
    ark_syscall6(ARK_SYS_LOG, (uintptr_t)message, sizeof message - 1, 0, 0, 0, 0);
    for (unsigned i = 0; i < 20; i++)
        ark_wait_ms(50);
    return 0;
}
