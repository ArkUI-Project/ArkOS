#ifndef ARK_LIQUID_GLASS_H
#define ARK_LIQUID_GLASS_H
#include <stdint.h>
#include <stdbool.h>
/* Native C port of Kyant's rounded rectangle lens and directional highlight.
 * Pixel centers, dimensions and parameters are physical pixels. */
typedef struct {
    float dx, dy, dispersion_x, dispersion_y, highlight, distance;
} LiquidLens;
void liquid_glass_lens(float x, float y, float width, float height, float radius,
                       float refraction_height, float refraction_amount, bool depth,
                       bool dispersion, LiquidLens *);
uint32_t liquid_glass_vibrancy(uint32_t);
void liquid_glass_gaussian_row(uint32_t *, const uint32_t *const rows[13], int count);
uint32_t liquid_glass_sample(const uint32_t *, int stride, int width, int height, int x_q8,
                             int y_q8);
uint32_t liquid_glass_dispersion(const uint32_t *, int stride, int width, int height, int x_q8,
                                 int y_q8, int dx_q8, int dy_q8);
typedef struct {
    float value, velocity, target;
    uint64_t at;
    bool active;
} LiquidSpring;
void liquid_spring_target(LiquidSpring *, float target, uint64_t millis);
bool liquid_spring_update(LiquidSpring *, uint64_t millis);
float liquid_glass_tanh(float);
#endif
