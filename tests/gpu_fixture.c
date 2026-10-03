/* Standalone driver diagnostic kernel; built only by gpu_test.py. */
#include "gpu.h"
static uint32_t pixels[1920 * 1200];
static BootInfo boot;
static void num(uint64_t n) {
    char b[24];
    uint_to_str(n, b);
    serial_write(b);
}
static void require(bool ok, const char *message) {
    if (!ok) {
        serial_write("[gpu-test] FAIL ");
        serial_write(message);
        serial_write("\n");
        for (;;)
            platform_idle();
    }
}
static void verify(void) {
    for (unsigned y = 0; y < boot.height; y++)
        for (unsigned x = 0; x < boot.width; x++) {
            uint32_t got = *(volatile uint32_t *)(uintptr_t)(boot.framebuffer +
                                                             (uint64_t)y * boot.pitch + x * 4);
            uint32_t c = pixels[y * boot.width + x];
            uint32_t want = (((c >> 16) & 255) << boot.red_pos) |
                            (((c >> 8) & 255) << boot.green_pos) | ((c & 255) << boot.blue_pos);
            if (got != want) {
                serial_write("[gpu-test] mismatch x=");
                num(x);
                serial_write(" y=");
                num(y);
                serial_write(" got=");
                num(got);
                serial_write(" want=");
                num(want);
                serial_write("\n");
                require(false, "framebuffer comparison");
            }
        }
}
void kernel_main(uint32_t magic, uint32_t info) {
    platform_init(magic, info, &boot);
    gpu_init(&boot);
    for (unsigned y = 0; y < boot.height; y++)
        for (unsigned x = 0; x < boot.width; x++)
            pixels[y * boot.width + x] = 0x254769;
    for (unsigned y = 64; y < 80; y++)
        for (unsigned x = 64; x < 96; x++)
            pixels[y * boot.width + x] = ((x + y) & 1) ? 0xabcdef : 0x765432;
    gpu_present(pixels, boot.width, 0);
    uint64_t deadline = platform_ticks() + 200;
    while (!strncmp(gpu_backend_name(), "SVGA II / waiting", 17) && platform_ticks() < deadline) {
        platform_idle();
        gpu_present(pixels, boot.width, 0);
    }
    require(strncmp(gpu_backend_name(), "SVGA II / waiting", 17) != 0,
            "display refresh did not arrive");
    bool accelerated = gpu_stats()->accelerated;
    verify();
    for (unsigned y = 64; y < 80; y++)
        for (unsigned x = 64; x < 96; x++)
            pixels[(y + 32) * boot.width + x + 128] = pixels[y * boot.width + x];
    gpu_present(pixels, boot.width, 0);
    verify();
    if (accelerated) {
        require(gpu_stats()->fill_commands > 0, "no RECT_FILL submitted");
        require(gpu_stats()->copy_commands > 0, "no retained tile RECT_COPY submitted");
    }
    /* Force an unaligned region update, then overwrite VRAM externally and prove
     * identical damage is still uploaded (software-cursor restore contract). */
    GpuRect damage = {13, 19, 23, 17};
    for (int y = 19; y < 36; y++)
        for (int x = 13; x < 36; x++)
            pixels[y * boot.width + x] = 0xdecade;
    gpu_present(pixels, boot.width, &damage);
    verify();
    *(volatile uint32_t *)(uintptr_t)(boot.framebuffer + 19 * boot.pitch + 13 * 4) = 0;
    gpu_present(pixels, boot.width, &damage);
    verify();
    /* Simulate root's software cursor: upload temporary pixels, restore the
     * clean canvas only in RAM, then a full present must erase the device cursor. */
    uint32_t saved[23 * 17];
    for (int y = 0; y < 17; y++)
        for (int x = 0; x < 23; x++) {
            saved[y * 23 + x] = pixels[(y + 19) * boot.width + x + 13];
            pixels[(y + 19) * boot.width + x + 13] = 0xffffff;
        }
    gpu_present(pixels, boot.width, &damage);
    for (int y = 0; y < 17; y++)
        for (int x = 0; x < 23; x++)
            pixels[(y + 19) * boot.width + x + 13] = saved[y * 23 + x];
    gpu_present(pixels, boot.width, 0);
    verify();
    /* Streaming a moving region must upload exactly its damage and invalidate
     * the old retained shadow before the next ordinary full presentation. */
    for (int y = 19; y < 36; y++)
        for (int x = 13; x < 36; x++)
            pixels[y * boot.width + x] = 0x773344;
    uint64_t uploaded = gpu_stats()->uploaded_pixels;
    gpu_present_animation_damage(pixels, boot.width, &damage);
    verify();
    require(gpu_stats()->uploaded_pixels - uploaded == 23 * 17,
            "animation damage became a full upload");
    for (int y = 0; y < 17; y++)
        for (int x = 0; x < 23; x++)
            pixels[(y + 19) * boot.width + x + 13] = saved[y * 23 + x];
    gpu_present(pixels, boot.width, 0);
    verify();
    GpuRect clipped = {-3, -5, 5, 7};
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2; x++)
            pixels[y * boot.width + x] = 0xcafefe;
    uploaded = gpu_stats()->uploaded_pixels;
    gpu_present_animation_damage(pixels, boot.width, &clipped);
    verify();
    require(gpu_stats()->uploaded_pixels - uploaded == 4, "animation damage clipping");
    /* Multiple all-solid frames exercise FIFO wrap and >1024 commands/service. */
    for (unsigned frame = 0; frame < 3; frame++) {
        for (unsigned i = 0; i < boot.width * boot.height; i++)
            pixels[i] = 0x123450 + frame;
        gpu_present(pixels, boot.width, 0);
        verify();
    }
    serial_write("[gpu-test] PASS backend=");
    serial_write(gpu_backend_name());
    serial_write(" fill=");
    num(gpu_stats()->fill_commands);
    serial_write(" copy=");
    num(gpu_stats()->copy_commands);
    serial_write(" upload_pixels=");
    num(gpu_stats()->uploaded_pixels);
    serial_write("\n");
    for (;;)
        platform_idle();
}
