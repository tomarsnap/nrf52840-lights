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

/*
 * Active pixel count — how many LEDs are physically connected.
 *
 * The DTS chain-length is the MAXIMUM the firmware supports; the active count
 * is set at runtime and persisted, so one image drives an 18, 36 or 72 pixel
 * chain. Data for pixels beyond the active count is still clocked out (the
 * driver always transmits a full frame) but simply falls off the end of a
 * shorter chain.
 */
int led_effects_pixel_count(void);      /* currently active count */
int led_effects_max_pixels(void);       /* DTS chain-length — the ceiling */

/* Set the active count and persist it. Takes effect on the next frame.
 * Returns -EINVAL if count is 0 or above the maximum. */
int led_effects_set_count(uint16_t count);

/* Drive a single solid color immediately, bypassing the effect thread.
 * For bring-up diagnostics. Returns the driver's update status. */
int led_effects_direct_fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri);

/* Clear the strip and light exactly one pixel. For bring-up diagnostics:
 * draws minimal current, so it separates "no data" from "brownout". */
int led_effects_direct_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri);
void led_effects_set_effect(led_effect_t effect);
void led_effects_set_color(uint8_t r, uint8_t g, uint8_t b);

/* Brightness is persisted, like the pixel count, and restored at boot.
 * Do not set it unconditionally at startup — that would overwrite the
 * value the user saved. */
void led_effects_set_brightness(uint8_t brightness);
uint8_t led_effects_get_brightness(void);

/*
 * Animation speed, 0-255 (WLED-style): 0 slowest, 255 fastest, 128 the default.
 * It has no fixed meaning — each effect decides how to use it, typically via
 * led_speed_delay() in led_animations.h. Persisted and restored at boot, like
 * brightness. Non-animating effects (solid, off) ignore it.
 */
void led_effects_set_speed(uint8_t speed);
uint8_t led_effects_get_speed(void);

/* Force the strip black regardless of effect, for a critical battery.
 * Not persisted — a reboot clears it. */
void led_effects_set_lockout(bool lockout);
bool led_effects_is_locked_out(void);
