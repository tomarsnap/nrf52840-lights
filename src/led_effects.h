#pragma once

#include <zephyr/kernel.h>
#include <stdint.h>

typedef enum {
    EFFECT_OFF       = 0,
    EFFECT_SOLID     = 1,
    EFFECT_RAINBOW   = 2,
    EFFECT_BREATHE   = 3,
    EFFECT_WORM      = 4,
    EFFECT_SPARKLE   = 5,   /* sparkle + travelling gradient, ring 1 randomised */
    EFFECT_AURORA    = 6,   /* slow flowing gradient, tinted by the global colour */
    EFFECT_METEOR    = 7,   /* twin meteors crossing, one per ring, mirrored      */
    EFFECT_HEARTBEAT = 8,   /* lub-dub brightness pulse in the global colour      */
    EFFECT_FIREFLY   = 9,   /* sparse independent twinkles fading in and out      */
    EFFECT_PINWHEEL  = 10,  /* counter-rotating colour wedges                     */
    EFFECT_CONFETTI  = 11,  /* random self-coloured pops (ignores global colour)  */
} led_effect_t;

/* Keep in sync with the enum above: the highest valid effect index, used to
 * range-check the "E" command. */
#define EFFECT_MAX  EFFECT_CONFETTI

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
led_effect_t led_effects_get_effect(void);

/* Current colour (the last "C" value). Reported by "?" so an app can sync its
 * UI on connect. Not persisted — cleared to the boot default on reset. */
void led_effects_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_effects_get_color(uint8_t *r, uint8_t *g, uint8_t *b);

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

/*
 * Auto colour cycling: when enabled, the global colour is re-randomised at each
 * effect's cycle boundary (see led_cycle_color() in led_animations.h), so the
 * colour drifts on its own instead of staying at the last "C" value. Persisted
 * and restored at boot, like speed. Effects that ignore the colour (rainbow) or
 * have no cycle (solid, off) are unaffected while the mode is on.
 */
void led_effects_set_auto_color(bool on);
bool led_effects_get_auto_color(void);

/*
 * Pick a fresh random colour and make it the current one. Called by
 * led_cycle_color() from an effect at its cycle boundary; not normally called
 * directly. Thread-safe.
 */
void led_effects_cycle_color(void);

/* Force the strip black regardless of effect, for a critical battery.
 * Not persisted — a reboot clears it. */
void led_effects_set_lockout(bool lockout);
bool led_effects_is_locked_out(void);

/*
 * ── Ring geometry ────────────────────────────────────────────────────────────
 *
 * The active pixel count is split evenly into LED_RINGS equal rings (see
 * led_animations.h). These report the resulting layout and let it be calibrated
 * to the physical mounting.
 */
int led_effects_ring_count(void);   /* rings mapped (1 if count is not divisible) */
int led_effects_ring_size(void);    /* pixels per ring                            */

/* Per-ring calibration readback (for the "?" report). top is a physical pixel
 * index; dir is +1 or -1. Returns -1 (top) / 0 (dir) for an invalid ring. */
int led_effects_ring_top(int ring);
int led_effects_ring_dir(int ring);

/*
 * Calibrate one ring: `top` is the physical pixel index sitting at the ring's
 * reference point, `dir` (+1/-1) its winding direction. Persisted. Returns
 * -EINVAL if the ring index is out of range, top is outside that ring, or dir
 * is not +/-1. Use led_effects_identify() to find the top pixel.
 */
int led_effects_set_ring_cal(int ring, int top, int dir);

/*
 * Diagnostic "identify" mode: light exactly one physical pixel (and blank the
 * rest) so a ring's top can be located. Overrides the running effect until a
 * new effect is selected; pass a negative index to turn it off. Not persisted.
 */
void led_effects_identify(int pixel);
