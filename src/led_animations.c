/*
 * LED animations — the effect renderers.
 *
 * This is the file to edit when working on effects. Each render_*() function
 * paints one frame into the caller's pixel buffer and returns the delay until
 * the next frame; led_render() dispatches to them by effect id. See
 * led_animations.h for the contract, and led_effects.c for the engine that
 * drives all of this (state, persistence, threading, the strip write) — none
 * of which an effect needs to touch.
 *
 * These functions run on the single effect thread, one at a time, so any
 * per-effect animation phase can live in a plain file-scope static with no
 * locking (see render_rainbow / render_breathe).
 */

#include "led_animations.h"

#include <string.h>
#include <stdint.h>

/* ── Colour helpers ──────────────────────────────────────────────────────── */

/* Simple HSV→RGB: hue 0-255, saturation and value fixed at max. */
static void hsv_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
{
    uint8_t region = hue / 43U;
    uint8_t rem    = (hue - region * 43U) * 6U;
    uint8_t p      = 0U;
    uint8_t q      = (uint16_t)255U * (255U - rem) >> 8U;
    uint8_t t      = (uint16_t)255U * rem >> 8U;

    switch (region) {
    case 0: *r = 255; *g = t;   *b = p;   break;
    case 1: *r = q;   *g = 255; *b = p;   break;
    case 2: *r = p;   *g = 255; *b = t;   break;
    case 3: *r = p;   *g = q;   *b = 255; break;
    case 4: *r = t;   *g = p;   *b = 255; break;
    default:*r = 255; *g = p;   *b = q;   break;
    }
}

/* ── Effects ─────────────────────────────────────────────────────────────── */
/*
 * Each renderer fills f->pixels[0 .. f->count-1] and returns its inter-frame
 * delay in milliseconds.
 */

static uint32_t render_off(const struct led_frame *f)
{
    memset(f->pixels, 0, f->count * sizeof(f->pixels[0]));
    return 100U;
}

static uint32_t render_solid(const struct led_frame *f)
{
    for (uint16_t i = 0; i < f->count; i++) {
        f->pixels[i].r = led_scale(f->r, f->brightness);
        f->pixels[i].g = led_scale(f->g, f->brightness);
        f->pixels[i].b = led_scale(f->b, f->brightness);
    }
    return 100U;
}

static uint32_t render_rainbow(const struct led_frame *f)
{
    /* Animation phase: advanced once per frame, retained across calls. */
    static uint8_t offset;

    /* Spread one full hue cycle over the ACTIVE pixels, so the gradient wraps
     * correctly whatever the ring size. */
    for (uint16_t i = 0; i < f->count; i++) {
        uint8_t hue = offset + (uint8_t)(i * 256U / f->count);
        uint8_t hr, hg, hb;

        hsv_to_rgb(hue, &hr, &hg, &hb);
        f->pixels[i].r = led_scale(hr, f->brightness);
        f->pixels[i].g = led_scale(hg, f->brightness);
        f->pixels[i].b = led_scale(hb, f->brightness);
    }
    offset++;

    /* Speed sets the frame rate: ~60 ms/frame at the slowest, ~4 ms flat out. */
    return led_speed_delay(f->speed, 4U, 60U);
}

static uint32_t render_breathe(const struct led_frame *f)
{
    /* Animation phase: position in the 0→255→0 brightness sweep. */
    static uint16_t step;

    /* Triangle wave 0→254→0 over 256 steps. */
    uint8_t wave = (step < 128U)
                 ? (uint8_t)(step * 2U)
                 : (uint8_t)((255U - step) * 2U);
    step = (step + 1U) & 0xFFU;

    uint8_t effective_bri = led_scale(wave, f->brightness);

    for (uint16_t i = 0; i < f->count; i++) {
        f->pixels[i].r = led_scale(f->r, effective_bri);
        f->pixels[i].g = led_scale(f->g, effective_bri);
        f->pixels[i].b = led_scale(f->b, effective_bri);
    }

    /* Speed sets how fast the breath sweeps: ~40 ms/step slow, ~2 ms fast. */
    return led_speed_delay(f->speed, 2U, 40U);
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

uint32_t led_render(const struct led_frame *f)
{
    switch (f->effect) {
    case EFFECT_OFF:     return render_off(f);
    case EFFECT_SOLID:   return render_solid(f);
    case EFFECT_RAINBOW: return render_rainbow(f);
    case EFFECT_BREATHE: return render_breathe(f);
    default:             return render_off(f);
    }
}
