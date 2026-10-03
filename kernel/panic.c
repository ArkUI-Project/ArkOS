/* Allocation-free fatal screen. No user compositor, storage or lock required. */
#include "ark.h"
#include "process.h"
#include "../user/font.h"
volatile unsigned ark_kernel_panicked;
static BootInfo screen;
extern bool gpu_panic_display(BootInfo *) __attribute__((weak));
extern void gpu_panic_publish(void) __attribute__((weak));
void kernel_panic_display_init(const BootInfo *b) {
    screen = *b;
}
static void text(int x, int y, const char *s, unsigned scale, uint32_t rgb) {
    uint32_t c = (((rgb >> 16) & 255) << screen.red_pos) |
                 (((rgb >> 8) & 255) << screen.green_pos) | ((rgb & 255) << screen.blue_pos);
    for (; *s; s++, x += (int)scale * 8) {
        unsigned ch = (uint8_t)*s;
        if (ch < 32 || ch > 126)
            ch = '?';
        for (unsigned yy = 0; yy < 16; yy++)
            for (unsigned xx = 0; xx < 8; xx++)
                if (font[ch - 32][yy] & (1u << xx))
                    for (unsigned a = 0; a < scale; a++)
                        for (unsigned b = 0; b < scale; b++) {
                            unsigned px = (unsigned)x + xx * scale + b,
                                     py = (unsigned)y + yy * scale + a;
                            if (px < screen.width && py < screen.height)
                                *(volatile uint32_t *)(uintptr_t)(screen.framebuffer +
                                                                  (uint64_t)py * screen.pitch +
                                                                  px * 4) = c;
                        }
    }
}
static void hex(char s[19], uint64_t v) {
    const char *h = "0123456789abcdef";
    s[0] = '0';
    s[1] = 'x';
    for (unsigned i = 0; i < 16; i++)
        s[i + 2] = h[(v >> (60 - i * 4)) & 15];
    s[18] = 0;
}
__attribute__((noreturn)) void kernel_panic(const char *reason, const InterruptFrame *f) {
    __asm__ volatile("cli" ::: "memory");
    if (__atomic_exchange_n(&ark_kernel_panicked, 1, __ATOMIC_SEQ_CST))
        for (;;)
            __asm__ volatile("hlt");
    serial_write("[panic] ");
    serial_write(reason);
    serial_write("\n");
    if (gpu_panic_display)
        (void)gpu_panic_display(&screen);
    if (screen.framebuffer && screen.width >= 640 && screen.width <= 1920 &&
        screen.height <= 1200 && screen.pitch >= screen.width * 4) {
        uint32_t bg =
            (0x16u << screen.red_pos) | (0x3du << screen.green_pos) | (0x86u << screen.blue_pos);
        for (unsigned y = 0; y < screen.height; y++)
            for (unsigned x = 0; x < screen.width; x++)
                *(volatile uint32_t *)(uintptr_t)(screen.framebuffer + (uint64_t)y * screen.pitch +
                                                  x * 4) = bg;
        int x = (int)screen.width / 8, y = (int)screen.height / 5;
        text(x, y, "ArkOS stopped safely", 2, 0xffffff);
        text(x, y + 62, "A fatal kernel exception stopped this system.", 1, 0xddeaff);
        text(x, y + 94, reason, 1, 0xffffff);
        char n[24];
        uint_to_str(platform_current_cpu(), n);
        text(x, y + 140, "CPU", 1, 0xb8d7ff);
        text(x + 60, y + 140, n, 1, 0xffffff);
        if (f) {
            text(x, y + 174, "RIP", 1, 0xb8d7ff);
            hex(n, f->rip);
            text(x + 60, y + 174, n, 1, 0xffffff);
            text(x, y + 208, "ERROR", 1, 0xb8d7ff);
            hex(n, f->error);
            text(x + 60, y + 208, n, 1, 0xffffff);
            uint64_t fault;
            __asm__ volatile("mov %%cr2,%0" : "=r"(fault));
            text(x, y + 242, "CR2", 1, 0xb8d7ff);
            hex(n, fault);
            text(x + 60, y + 242, n, 1, 0xffffff);
        }
        text(x, y + 306, "Record this screen and the serial log, then restart.", 1, 0xffffff);
        text(x, y + 335, "Pending changes were not written to disk.", 1, 0xb8d7ff);
        if (gpu_panic_publish)
            gpu_panic_publish();
    }
    for (;;)
        __asm__ volatile("hlt");
}
