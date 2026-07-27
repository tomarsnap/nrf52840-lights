#pragma once

#include <stdint.h>

#include <zephyr/drivers/led_strip.h>

#include "led_effects.h" /* led_effect_t */

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
