#ifndef ARK_GPU_H
#define ARK_GPU_H
#include "ark.h"
/* Rectangles and pixel stride use pixels; canvas pixels are 0x00RRGGBB. */
typedef struct {
    int x, y, w, h;
} GpuRect;
typedef struct {
    bool accelerated, hardware_cursor, shader_accelerated;
    uint32_t capabilities;
    uint64_t frames, fill_commands, copy_commands, update_commands;
    uint64_t filled_pixels, copied_pixels, uploaded_pixels, cursor_updates;
} GpuStats;
/* Called once after platform_init, before desktop caches framebuffer metadata.
 * True means acceleration verified now. A headless display can remain pending
 * until its first refresh; later presents finish initialization automatically. */
bool gpu_init(BootInfo *boot);
/* NULL damage means a full frame. Complex pixels are still CPU rasterized. */
void gpu_present(const uint32_t *canvas, unsigned stride, const GpuRect *damage);
/* Full-frame streaming for cached animation frames: no tile hashing or
 * retained-pixel copy. The next ordinary present rebuilds its tile cache. */
void gpu_present_animation(const uint32_t *canvas, unsigned stride);
/* Stream a clipped animation rectangle without retaining/hash-copying pixels. */
void gpu_present_animation_damage(const uint32_t *canvas, unsigned stride, const GpuRect *damage);
/* Returns true when a device cursor owns pointer drawing (also when hidden). */
bool gpu_cursor_move(int x, int y, bool visible);
void gpu_cursor_set_scale(unsigned scale);
void gpu_cursor_set_shape(unsigned shape);
const char *gpu_backend_name(void);
const GpuStats *gpu_stats(void);
#endif
