#pragma once

#include <stdint.h>
#include <string.h> /* memset, size_t — for led_clear() */

#include <zephyr/drivers/led_strip.h>

#include "led_effects.h" /* led_effect_t */

/*
 * ── Ring geometry ────────────────────────────────────────────────────────────
 *
 * The strip is physically one WS2812 chain, but it is mounted as a number of
 * equal-size rings wired in series (ring 0 = pixels [0,size), ring 1 =
 * [size,2*size), ...). The engine owns this geometry; effects address rings by
 * LOGICAL position and never see a physical index.
 *
 * How many rings is chosen at RUNTIME (the "R" command, persisted) — like the
 * pixel count — so one image drives a single- or dual-ring build. LED_MAX_RINGS
 * is the compile-time ceiling that sizes the per-frame and calibration arrays;
 * the active count is always <= it. ring_count in each frame is the number of
 * rings actually mapped this frame.
 *
 * base/size come from the wiring (the active pixel count split evenly across
 * the active rings). top/dir are the physical-mounting calibration, set over
 * BLE with the "K" command and persisted: `top` is the physical pixel sitting
 * at the ring's reference point ("12 o'clock"), and `dir` is which way the
 * physical index moves as a logical position increases (+1 or -1). Calibrating
 * two rings with opposite `dir` makes led_all_set() come out as a mirror image;
 * the same `dir` makes it an identical copy.
 */
#define LED_MAX_RINGS 4

struct led_ring {
    uint16_t base;   /* physical index of the ring's first pixel in the chain */
    uint16_t size;   /* pixels in the ring                                    */
    uint16_t top;    /* physical index of the ring's reference ("top") pixel  */
    int8_t   dir;    /* +1 / -1: physical travel as logical position rises    */
};

/*
 * ── The animation contract ───────────────────────────────────────────────────
 *
 * This header is the entire boundary between the effect engine and the effect
 * code. If you are writing a new animation you only need this file and
 * led_animations.c — the engine (led_effects.c) handles state, persistence,
 * threading, the battery lockout and the actual strip write, and none of that
 * has to change to add an effect.
 *
 * The engine calls led_render() once per frame from a single thread. It has
 * already snapshotted the live state under its mutex, so a `struct led_frame`
 * is an immutable, race-free view of what to draw right now.
 */

/*
 * Everything an effect needs to render one frame.
 *
 * A render function fills pixels[0 .. count-1] and returns. Pixels past `count`
 * are blanked by the engine, so an effect only ever touches the active range
 * and never needs to know the compile-time maximum.
 */
struct led_frame {
    struct led_rgb *pixels;     /* buffer to fill, indices [0, count)      */
    uint16_t        count;      /* number of active pixels                 */
    led_effect_t    effect;     /* which animation to render               */
    uint8_t         r, g, b;    /* current colour (for solid/breathe)      */
    uint8_t         brightness; /* master brightness, 0-255                */
    uint8_t         speed;      /* animation speed, 0-255 (WLED-style)     */
    bool            auto_color; /* re-randomise colour each cycle (see below) */

    /* Geometry snapshot — use the drawing helpers below (led_ring_set,
     * led_all_set, ...) rather than touching this directly. */
    uint8_t         ring_count;         /* rings mapped this frame (<= LED_MAX_RINGS) */
    struct led_ring rings[LED_MAX_RINGS];
};

/*
 * Render one frame of `f->effect` into `f->pixels`, and return how long the
 * engine should wait before asking for the next frame, in milliseconds. That
 * return value is the effect's speed control — a slow fade returns a large
 * delay, a fast chase a small one.
 *
 * To add an effect:
 *   1. add a value to led_effect_t in led_effects.h,
 *   2. write a render_*() function in led_animations.c,
 *   3. add a case for it to the switch in led_render().
 * Nothing outside led_animations.c and that enum needs to change.
 */
uint32_t led_render(const struct led_frame *f);

/*
 * Scale a 0-255 colour channel by a 0-255 brightness. Shared with the engine's
 * diagnostic fills, so it lives here rather than being duplicated.
 */
static inline uint8_t led_scale(uint8_t val, uint8_t brightness)
{
    return (uint16_t)val * brightness / 255U;
}

/*
 * Compile-time ceiling for per-effect per-pixel state arrays (sparkle decay,
 * firefly lifetimes, confetti hues). Must be >= the DTS chain_length — the
 * engine's own pixel buffer uses that same length — so an effect's state array
 * is never indexed past its end whatever the active count. Kept generous; the
 * cost is one byte of BSS per pixel per array, nothing on this SoC.
 */
#ifndef LED_MAX_PIXELS
#define LED_MAX_PIXELS 256
#endif

/*
 * HSV→RGB at full saturation and value: map a 0-255 hue to an RGB triple. Used
 * by the rainbow effect and by the engine's auto-colour cycling to pick vivid,
 * never-muddy colours (a plain random RGB is often dim or grey). Shared here
 * rather than duplicated, like led_scale.
 */
static inline void led_hsv_to_rgb(uint8_t hue, uint8_t *r, uint8_t *g, uint8_t *b)
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

/*
 * Approximate the 0-255 hue of an RGB triple (saturation and value discarded) —
 * the rough inverse of led_hsv_to_rgb. Lets an effect start flowing around the
 * hue wheel from the user's global colour instead of an arbitrary hue, so the
 * picker still tints effects that synthesise their own gradient (aurora,
 * fireflies, pinwheel). A grey/white input has no hue and returns 0.
 */
static inline uint8_t led_rgb_hue(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    uint8_t min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    uint8_t delta = (uint8_t)(max - min);

    if (delta == 0U) {
        return 0U;   /* achromatic */
    }

    /* 43 ≈ 60° on the 0-255 wheel; each primary owns a 120° (85-unit) sector. */
    int32_t hue;
    if (max == r) {
        hue = 0   + (int32_t)43 * ((int)g - (int)b) / delta;
    } else if (max == g) {
        hue = 85  + (int32_t)43 * ((int)b - (int)r) / delta;
    } else {
        hue = 171 + (int32_t)43 * ((int)r - (int)g) / delta;
    }
    if (hue < 0) {
        hue += 256;
    }
    return (uint8_t)(hue & 0xFF);
}

/*
 * Blend colour a→b by t/255 (0 = all a, 255 = all b), per channel — the
 * two-colour gradient primitive. led_scale only fades a single colour toward
 * black; this interpolates between any two, which is what a gradient "around
 * the ring" or a "push toward white" sparkle needs.
 */
static inline struct led_rgb led_lerp(struct led_rgb a, struct led_rgb b, uint8_t t)
{
    struct led_rgb o = {0};   /* zero any optional scratch field, like a literal */

    o.r = (uint8_t)(((uint16_t)a.r * (255U - t) + (uint16_t)b.r * t) / 255U);
    o.g = (uint8_t)(((uint16_t)a.g * (255U - t) + (uint16_t)b.g * t) / 255U);
    o.b = (uint8_t)(((uint16_t)a.b * (255U - t) + (uint16_t)b.b * t) / 255U);
    return o;
}

/*
 * Turn a 0-255 speed (f->speed) into a per-frame delay in milliseconds. This is
 * the usual way an effect consumes speed: speed 0 is slowest and returns
 * slow_ms, speed 255 is fastest and returns fast_ms, scaling linearly between —
 * the default speed of 128 lands near the middle of the range you pass. Give it
 * whatever delay range feels right for the effect. Requires slow_ms >= fast_ms.
 *
 * Effects are free to use speed differently (e.g. to control how far an
 * animation advances per frame instead of the frame rate); this helper just
 * covers the common case. Effects that do not animate (solid, off) can ignore
 * speed entirely and return a fixed delay.
 */
static inline uint32_t led_speed_delay(uint8_t speed,
                                       uint32_t fast_ms, uint32_t slow_ms)
{
    return slow_ms - (uint32_t)(slow_ms - fast_ms) * speed / 255U;
}

/*
 * How many animation steps to advance this frame for a given speed — the second
 * half of the speed story, for when led_speed_delay() alone runs out of road.
 *
 * The strip's refresh floor (~12 ms/frame, driven by the driver's reset gap)
 * caps the frame rate, so past a certain point shortening the delay does nothing
 * and the only way to animate faster is to move further each frame. This returns
 * 1 for all speeds up to the 128 midpoint (so the delay is the sole control
 * through the low and normal range, and motion stays smooth), then ramps
 * linearly to max_step at speed 255. Use it for an effect's phase advance:
 *
 *     offset += led_speed_step(f->speed, 8);
 *
 * max_step is per-effect headroom — how many units/hue/pixels a single frame may
 * jump at full speed. Keep it small (a handful) or fast motion turns choppy.
 */
static inline uint16_t led_speed_step(uint8_t speed, uint16_t max_step)
{
    if (speed <= 128U || max_step <= 1U) {
        return 1U;
    }
    return (uint16_t)(1U + (uint32_t)(max_step - 1U) * (speed - 128U) / 127U);
}

/* ── Drawing API ─────────────────────────────────────────────────────────────
 *
 * How effects paint. All of these write into the one shared frame buffer, so
 * there is no per-ring memory. Choose the call by intent:
 *
 *   led_all_set  — same logical position on EVERY ring (the mirror primitive).
 *   led_ring_set — a specific ring, for effects that differ between rings.
 *   led_px_set   — a raw physical pixel, for whole-strip effects.
 *
 * led_ring_len()/led_ring_count() let an effect be written once and work for
 * any ring size or a single-ring build.
 */

/* Number of rings currently mapped (1 if the geometry could not be split). */
static inline uint16_t led_ring_count(const struct led_frame *f)
{
    return f->ring_count;
}

/* Pixels in one ring. Rings are equal size, so ring 0 speaks for all; with no
 * rings mapped this is the whole active strip. */
static inline uint16_t led_ring_len(const struct led_frame *f)
{
    return f->ring_count ? f->rings[0].size : f->count;
}

/* Map a logical position on a ring to its physical pixel index, applying the
 * ring's calibrated top offset and winding direction, wrapping around. */
static inline uint16_t led__ring_phys(const struct led_ring *ring, uint16_t p)
{
    uint16_t size = ring->size;
    uint16_t off  = ring->top - ring->base;                 /* 0 .. size-1 */
    int32_t  idx  = (int32_t)off + (int32_t)ring->dir * (int32_t)(p % size);

    idx %= (int32_t)size;
    if (idx < 0) {
        idx += size;
    }
    return (uint16_t)(ring->base + (uint16_t)idx);
}

/* Set logical position p (0 .. led_ring_len-1) on one ring. Out-of-range ring
 * indices are ignored, so an effect written for two rings is harmless on a
 * single-ring build. */
static inline void led_ring_set(const struct led_frame *f, int ring,
                                uint16_t p, struct led_rgb col)
{
    if (ring < 0 || ring >= f->ring_count) {
        return;
    }
    const struct led_ring *r = &f->rings[ring];

    if (r->size != 0U) {
        f->pixels[led__ring_phys(r, p)] = col;
    }
}

/* Set logical position p on every ring — the same pattern on all of them. With
 * opposite per-ring `dir` calibration this is a true mirror. */
static inline void led_all_set(const struct led_frame *f, uint16_t p,
                               struct led_rgb col)
{
    for (uint8_t i = 0; i < f->ring_count; i++) {
        led_ring_set(f, (int)i, p, col);
    }
}

/* Set a raw physical pixel — for effects that treat the whole chain as one
 * continuous strip and do not care about ring boundaries. */
static inline void led_px_set(const struct led_frame *f, uint16_t i,
                              struct led_rgb col)
{
    if (i < f->count) {
        f->pixels[i] = col;
    }
}

/*
 * Auto colour cycling. When the user has enabled the mode (f->auto_color is
 * set), advance the global colour to a fresh random one; otherwise do nothing.
 *
 * An effect calls this at a natural CYCLE boundary — the instant its animation
 * returns to where it started (a rainbow wrap, a completed breath, one worm
 * lap). The new colour lands on the next frame, so it changes in step with the
 * motion rather than jerking mid-stroke, and pick the boundary where the strip
 * is momentarily dark (e.g. the bottom of a breath) for a seamless swap.
 *
 * Always safe to call: it is a no-op when the mode is off. Effects that do not
 * read the global colour at all (rainbow) simply never call it.
 */
static inline void led_cycle_color(const struct led_frame *f)
{
    if (f->auto_color) {
        led_effects_cycle_color();
    }
}

/* Blank the active strip. Effects that light only a few pixels (a worm, a
 * comet) call this first so the previous frame does not smear. */
static inline void led_clear(const struct led_frame *f)
{
    memset(f->pixels, 0, (size_t)f->count * sizeof(f->pixels[0]));
}
