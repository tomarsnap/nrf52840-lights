#pragma once

#include <zephyr/kernel.h>
#include <stdint.h>

typedef enum {
    EFFECT_OFF     = 0,
    EFFECT_SOLID   = 1,
    EFFECT_RAINBOW = 2,
    EFFECT_BREATHE = 3,
} led_effect_t;

/* Returns 0 on success, -ENODEV if the strip device is not ready.
 * The effect thread only starts if this returns 0. */
int led_effects_init(void);

/* Start the effect thread. Separate from init: the thread immediately drives
 * the whole strip, so the caller controls when that happens. */
void led_effects_start(void);

/* Number of pixels the strip is configured for (DTS chain-length). */
int led_effects_pixel_count(void);

/* Drive a single solid color immediately, bypassing the effect thread.
 * For bring-up diagnostics. Returns the driver's update status. */
int led_effects_direct_fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri);

/* Clear the strip and light exactly one pixel. For bring-up diagnostics:
 * draws minimal current, so it separates "no data" from "brownout". */
int led_effects_direct_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri);
void led_effects_set_effect(led_effect_t effect);
void led_effects_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_effects_set_brightness(uint8_t brightness);
