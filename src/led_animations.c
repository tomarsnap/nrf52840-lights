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

#include <zephyr/random/random.h>   /* sys_rand32_get — sparkle/firefly/confetti RNG */

/* ── Local helpers ───────────────────────────────────────────────────────── */

/*
 * Fast 8-bit sine: angle 0-255 spans one full period, output 0-255 with the
 * zero-crossing at 128. A parabolic approximation — no float, no table, smooth
 * enough for the aurora's soft bands and the heartbeat easing. Shape: a hump up
 * over the first half of the period, a matching dip over the second.
 */
static uint8_t sin8(uint8_t angle)
{
    bool    neg = angle >= 128U;             /* second half mirrors the first */
    uint8_t a   = neg ? (uint8_t)(angle - 128U) : angle;   /* 0..127 */
    uint16_t y  = (uint16_t)a * (128U - a);  /* parabola, peaks 4096 at a=64 */
    uint8_t  mag = (uint8_t)(y >> 5);        /* 0..128 */

    if (mag > 127U) {
        mag = 127U;                          /* clamp the single peak sample */
    }
    return neg ? (uint8_t)(128U - mag) : (uint8_t)(128U + mag);
}

/*
 * Lub-dub brightness envelope for the heartbeat, one full beat: a tall bump
 * (the "lub"), a short gap, a smaller bump (the "dub"), then a long rest at
 * zero. The strip is dark across the rest, so a colour swap at the wrap is
 * seamless. Stepped through by render_heartbeat.
 */
static const uint8_t heartbeat_env[] = {
    /* lub  */ 0,  30, 120, 220, 255, 205, 120, 45,
    /* gap  */ 10,  0,
    /* dub  */ 0,  40, 130, 175, 130,  60, 15,
    /* rest */ 0,   0,   0,   0,   0,   0,  0,  0, 0, 0, 0,
};

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

    const uint16_t len = led_ring_len(f);
    if (len == 0U) {
        return 60U;
    }

    /*
     * One FULL hue cycle per RING, addressed in logical positions. A ring is a
     * physical loop — its first and last pixels sit next to each other — so the
     * gradient must span the whole colour wheel across the ring for those two
     * neighbours to share a hue and wrap seamlessly. Spreading a single cycle
     * over the entire chain (the old approach) put only half the wheel on each
     * ring, leaving a jarring half-wheel jump at every ring seam. Drawing per
     * ring via the geometry engine also aligns both eyes to their calibrated
     * tops and winding direction.
     */
    for (int ring = 0; ring < led_ring_count(f); ring++) {
        for (uint16_t p = 0; p < len; p++) {
            uint8_t hue = offset + (uint8_t)(p * 256U / len);
            uint8_t hr, hg, hb;

            led_hsv_to_rgb(hue, &hr, &hg, &hb);
            struct led_rgb col = {
                .r = led_scale(hr, f->brightness),
                .g = led_scale(hg, f->brightness),
                .b = led_scale(hb, f->brightness),
            };
            led_ring_set(f, ring, p, col);
        }
    }
    /* Speed shortens the frame delay through the low/normal range, then (past
     * the midpoint, once the delay hits the refresh floor) spins the hue faster
     * per frame so max speed is genuinely fast. */
    offset += led_speed_step(f->speed, 8U);

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

    uint16_t prev = step;
    step = (uint16_t)((step + led_speed_step(f->speed, 6U)) & 0xFFU);
    if (step < prev) {
        /* Wrapped past 255 back to the bottom of the breath — one full cycle,
         * and the strip is momentarily dark, so a colour swap here is seamless. */
        led_cycle_color(f);
    }

    uint8_t effective_bri = led_scale(wave, f->brightness);

    for (uint16_t i = 0; i < f->count; i++) {
        f->pixels[i].r = led_scale(f->r, effective_bri);
        f->pixels[i].g = led_scale(f->g, effective_bri);
        f->pixels[i].b = led_scale(f->b, effective_bri);
    }

    /* Speed sets how fast the breath sweeps: ~40 ms/step slow, ~2 ms fast. */
    return led_speed_delay(f->speed, 2U, 40U);
}

/*
 * Two worms, one per ring, crawling in opposite directions — the worked
 * example for the per-ring drawing API. Because it addresses each ring
 * separately with led_ring_set(), the geometry engine places each worm using
 * that ring's own calibrated top and winding direction, so they stay visually
 * aligned however the rings are physically mounted. On a single-ring build the
 * ring-1 writes are simply ignored and one worm crawls.
 *
 * To make it a MIRROR instead (one worm reflected onto both rings), the two
 * led_ring_set() calls collapse to a single led_all_set(f, p0, col).
 */
static uint32_t render_worm(const struct led_frame *f)
{
    static uint16_t head;                 /* advances one pixel per frame */

    const uint16_t len  = led_ring_len(f);   /* pixels in one ring */
    const uint16_t tail = 5U;                /* worm length, head included */

    if (len == 0U) {
        return 50U;
    }

    led_clear(f);                            /* dark background; worm lights a few */

    /* Ring 0 head runs forward; ring 1 head runs the opposite way, so the two
     * worms chase around in mirror directions ("opposite timing"). */
    uint16_t head0 = head;
    uint16_t head1 = (uint16_t)((len - head % len) % len);

    for (uint16_t t = 0; t < tail && t < len; t++) {
        /* Head is brightest; the tail fades out behind it. */
        uint8_t fade = (uint8_t)(255U - t * (255U / tail));
        uint8_t bri  = led_scale(fade, f->brightness);
        struct led_rgb col = {
            .r = led_scale(f->r, bri),
            .g = led_scale(f->g, bri),
            .b = led_scale(f->b, bri),
        };

        led_ring_set(f, 0, (uint16_t)((head0 + len - t) % len), col); /* tail trails +1 */
        led_ring_set(f, 1, (uint16_t)((head1 + t) % len),       col); /* tail trails -1 */
    }

    uint16_t prev = head;
    head = (uint16_t)((head + led_speed_step(f->speed, 4U)) % len);
    if (head < prev) {
        led_cycle_color(f);   /* completed one lap → next lap gets a new colour */
    }

    return led_speed_delay(f->speed, 8U, 80U);
}

/*
 * Sparkle Comet — the headline sparkle + travelling gradient.
 *
 * Each ring carries a colour gradient wrapped once around it, with a brighter
 * "comet" arc sweeping around; on top, individual pixels flare toward white and
 * fade out (the sparkle). Ring 0's gradient is the user's global colour. Ring 1
 * only diverges when auto-colour (A1) is on: it then derives a RANDOMISED
 * companion hue, refreshed each lap, so the two eyes shimmer in related but
 * different colours. With auto-colour off the user picked a specific colour, so
 * ring 1 simply mirrors ring 0 — a second, independently drifting eye would
 * look wrong against a colour the user deliberately chose.
 *
 * Structure is two passes: draw the gradient+comet per ring in LOGICAL
 * positions (so the geometry engine aligns both eyes), then a sparkle overlay
 * over PHYSICAL pixels that reads each pixel back and pushes it toward white by
 * its decaying spark value. Keeping the sparkle physical sidesteps the
 * logical↔physical mismatch and gives an even ambient glitter.
 */
static uint32_t render_sparkle(const struct led_frame *f)
{
    static uint16_t head;                   /* comet head, logical position   */
    static uint8_t  spark[LED_MAX_PIXELS];  /* per-physical-pixel glint level */
    static uint8_t  companion_hue;          /* ring 1's randomised anchor hue */

    const uint16_t len = led_ring_len(f);
    if (len == 0U) {
        return 50U;
    }

    const struct led_rgb white = { .r = 255, .g = 255, .b = 255 };
    const uint8_t decay = 24U;              /* higher = shorter sparkle tails */

    /* 1. decay existing glints across the whole active chain */
    for (uint16_t i = 0; i < f->count; i++) {
        spark[i] = (spark[i] > decay) ? (uint8_t)(spark[i] - decay) : 0U;
    }

    /* 2. per-ring gradient + travelling comet, in logical positions */
    for (int ring = 0; ring < led_ring_count(f); ring++) {
        struct led_rgb anchor;

        if (ring == 0 || !f->auto_color) {
            /* ring 0 always, and ring 1 too unless auto-colour is drifting it */
            anchor = (struct led_rgb){ .r = f->r, .g = f->g, .b = f->b };
        } else {
            uint8_t cr, cg, cb;
            led_hsv_to_rgb(companion_hue, &cr, &cg, &cb);
            anchor = (struct led_rgb){ .r = cr, .g = cg, .b = cb };
        }
        struct led_rgb tail = { .r = anchor.r / 4U,
                                .g = anchor.g / 4U,
                                .b = anchor.b / 4U };   /* gradient into shadow */

        for (uint16_t p = 0; p < len; p++) {
            struct led_rgb col = led_lerp(anchor, tail, (uint8_t)(p * 255U / len));

            /* comet: brighten a short arc trailing the head, wrap-aware */
            uint16_t d = (uint16_t)((p + len - (head % len)) % len);
            if (d < 5U) {
                col = led_lerp(col, anchor, (uint8_t)(255U - d * 51U));
            }

            col.r = led_scale(col.r, f->brightness);
            col.g = led_scale(col.g, f->brightness);
            col.b = led_scale(col.b, f->brightness);
            led_ring_set(f, ring, p, col);
        }
    }

    /*
     * 3. sparkle overlay over physical pixels. Seed CLUSTERS rather than lone
     * pixels: when a pixel is chosen, light it and its two neighbours (a
     * five-pixel core with a brighter middle), so each glint reads as a chunky
     * twinkle instead of a single-LED dot. The seed probability is lowered to
     * match, since one seed now paints several pixels.
     */
    for (uint16_t i = 0; i < f->count; i++) {
        if ((sys_rand32_get() % 140U) == 0U) {
            /* neighbour falloff within the cluster; centre burns brightest */
            static const uint8_t cluster[5] = { 150U, 220U, 255U, 220U, 150U };
            for (int k = -2; k <= 2; k++) {
                int j = (int)i + k;
                if (j < 0 || j >= (int)f->count) {
                    continue;
                }
                uint8_t v = cluster[k + 2];
                if (v > spark[j]) {
                    spark[j] = v;
                }
            }
        }
    }
    for (uint16_t i = 0; i < f->count; i++) {
        if (spark[i] != 0U) {
            f->pixels[i] = led_lerp(f->pixels[i], white,
                                    led_scale(spark[i], f->brightness));
        }
    }

    uint16_t prev = head;
    head = (uint16_t)((head + led_speed_step(f->speed, 4U)) % len);
    if (head < prev) {
        /* one lap → re-randomise ring 1 and drift the global colour if auto */
        companion_hue += (uint8_t)(40U + (sys_rand32_get() % 176U));
        led_cycle_color(f);
    }

    return led_speed_delay(f->speed, 6U, 70U);
}

/*
 * Aurora Drift — a slow flowing gradient, like northern lights. Two sine waves
 * of different wavelength (one for hue, one for brightness) drift over each
 * ring; ring 1 is phase-shifted a third of a turn so the two eyes never move in
 * lockstep. The hue wanders around the user's global colour (via led_rgb_hue),
 * so the picker tints the whole aurora. Pure function of position and time — no
 * per-pixel state.
 */
static uint32_t render_aurora(const struct led_frame *f)
{
    static uint16_t t;                      /* time phase, wraps at 256 */

    const uint16_t len = led_ring_len(f);
    if (len == 0U) {
        return 50U;
    }

    uint8_t base = led_rgb_hue(f->r, f->g, f->b);

    for (int ring = 0; ring < led_ring_count(f); ring++) {
        uint8_t phase = (ring == 0) ? 0U : (uint8_t)(len / 3U);

        for (uint16_t p = 0; p < len; p++) {
            uint8_t pos = (uint8_t)(((uint32_t)(p + phase) * 256U) / len);
            uint8_t w1  = sin8((uint8_t)(pos * 2U + (uint8_t)t));
            uint8_t w2  = sin8((uint8_t)(pos * 3U - (uint8_t)(t * 2U)));

            uint8_t hue = (uint8_t)((int)base + ((int)w1 - 128) / 4);
            uint8_t val = (uint8_t)(150U + (w2 >> 2));   /* 150..213: gentle shimmer */

            uint8_t hr, hg, hb;
            led_hsv_to_rgb(hue, &hr, &hg, &hb);
            uint8_t bri = led_scale(val, f->brightness);
            struct led_rgb col = { .r = led_scale(hr, bri),
                                   .g = led_scale(hg, bri),
                                   .b = led_scale(hb, bri) };
            led_ring_set(f, ring, p, col);
        }
    }

    uint16_t prev = t;
    /* Smooth sinusoidal field, so it takes a brisk phase step (6) and a
     * near-floor frame rate at the top without any strobing — the low half of
     * the range still crawls (step 1, up to the 120 ms slow end). */
    t = (uint16_t)((t + led_speed_step(f->speed, 6U)) & 0xFFU);
    if (t < prev) {
        led_cycle_color(f);   /* one slow cycle → drift colour if auto */
    }

    return led_speed_delay(f->speed, 12U, 120U);
}

/*
 * Heartbeat — both eyes pulse in the global colour with a cardiac lub-dub
 * rhythm, stepping through heartbeat_env[]. No motion around the ring, just a
 * whole-strip brightness envelope; symmetric across the eyes by construction.
 */
static uint32_t render_heartbeat(const struct led_frame *f)
{
    static uint16_t phase;

    const uint16_t n = (uint16_t)ARRAY_SIZE(heartbeat_env);
    uint8_t wave = heartbeat_env[phase % n];
    uint8_t bri  = led_scale(wave, f->brightness);
    struct led_rgb col = { .r = led_scale(f->r, bri),
                           .g = led_scale(f->g, bri),
                           .b = led_scale(f->b, bri) };

    for (uint16_t i = 0; i < f->count; i++) {
        f->pixels[i] = col;
    }

    uint16_t prev = phase;
    phase = (uint16_t)((phase + led_speed_step(f->speed, 3U)) % n);
    if (phase < prev) {
        led_cycle_color(f);   /* end of the rest, strip dark → seamless swap */
    }

    return led_speed_delay(f->speed, 4U, 30U);
}

/*
 * Fireflies — a dark field with a handful of soft points that ignite at random,
 * glow, and fade slowly, each independent on either eye. Colour is the global
 * colour with a small per-firefly hue jitter. Per-pixel state: a brightness and
 * a hue, both physical-indexed.
 */
static uint32_t render_firefly(const struct led_frame *f)
{
    static uint8_t fly[LED_MAX_PIXELS];      /* per-pixel glow level */
    static uint8_t fly_hue[LED_MAX_PIXELS];  /* per-pixel hue        */
    static uint16_t frames;                  /* for the auto-colour tick */

    /* Past the speed midpoint, fade and ignite faster so the top of the range
     * is genuinely brisk: the ~12 ms refresh floor caps the frame rate, so
     * extra speed has to come from doing more per frame (led_speed_step returns
     * 1 up to the midpoint, so the default feel is unchanged). */
    uint16_t sp = led_speed_step(f->speed, 5U);   /* 1 → 5 across the top half */
    const uint8_t  decay = (uint8_t)(6U * sp);    /* 6 → 30: quicker glow-down  */
    const uint32_t seed  = 400U / sp;             /* 400 → 80: more ignitions   */

    led_clear(f);

    uint8_t base = led_rgb_hue(f->r, f->g, f->b);

    /* Fade everyone first, so ignition below only sees genuinely dark pixels. */
    for (uint16_t i = 0; i < f->count; i++) {
        fly[i] = (fly[i] > decay) ? (uint8_t)(fly[i] - decay) : 0U;
    }

    /* Rarely ignite a new firefly on a dark pixel — not a lone LED but a soft
     * glow a few pixels wide, tapering from the centre, so it has some body. As
     * it fades the dim edges reach 0 first, so the glow shrinks like a real one.
     * Seeding a cluster lights its neighbours, which keeps them from igniting
     * their own centre next to this one, so the fireflies stay distinct. */
    static const uint8_t profile[5] = { 64U, 160U, 255U, 160U, 64U };
    for (uint16_t i = 0; i < f->count; i++) {
        if (fly[i] != 0U || (sys_rand32_get() % seed) != 0U) {
            continue;
        }
        uint8_t peak   = (uint8_t)(140U + (sys_rand32_get() % 116U));  /* 140..255 */
        int     jitter = (int)(sys_rand32_get() % 31U) - 15;           /* ±15 hue  */
        uint8_t hue    = (uint8_t)((int)base + jitter);

        for (int k = -2; k <= 2; k++) {
            int j = (int)i + k;
            if (j < 0 || j >= (int)f->count) {
                continue;
            }
            uint8_t lvl = (uint8_t)((uint16_t)peak * profile[k + 2] / 255U);
            if (lvl > fly[j]) {
                fly[j]     = lvl;
                fly_hue[j] = hue;
            }
        }
    }

    for (uint16_t i = 0; i < f->count; i++) {
        if (fly[i] != 0U) {
            uint8_t hr, hg, hb;
            led_hsv_to_rgb(fly_hue[i], &hr, &hg, &hb);
            uint8_t bri = led_scale(fly[i], f->brightness);
            f->pixels[i] = (struct led_rgb){ .r = led_scale(hr, bri),
                                             .g = led_scale(hg, bri),
                                             .b = led_scale(hb, bri) };
        }
    }

    /* no natural lap boundary, so tick the auto-colour drift on a timer */
    if (++frames >= 300U) {
        frames = 0U;
        led_cycle_color(f);
    }

    return led_speed_delay(f->speed, 8U, 90U);
}

/*
 * Pinwheel — each eye is split into SEG equal wedges of evenly-spaced hues that
 * rotate steadily; ring 1 counter-rotates for a striking two-eye effect. Ring
 * 0's hues are anchored to the global colour, ring 1's to a randomised
 * companion hue refreshed each rotation.
 */
static uint32_t render_pinwheel(const struct led_frame *f)
{
    static uint16_t rot;
    static uint8_t  companion_hue;

    const uint16_t len = led_ring_len(f);
    const uint8_t  seg = 3U;                 /* wedges per ring */
    if (len == 0U) {
        return 50U;
    }

    uint8_t base0 = led_rgb_hue(f->r, f->g, f->b);

    for (int ring = 0; ring < led_ring_count(f); ring++) {
        /* Ring 1 gets its own drifting hue only under auto-colour; otherwise it
         * shares ring 0's colour and just counter-rotates, so A0 stays steady. */
        uint8_t  base = (ring == 0 || !f->auto_color) ? base0 : companion_hue;
        uint16_t rr   = (ring == 0) ? rot : (uint16_t)((len - rot % len) % len);

        for (uint16_t p = 0; p < len; p++) {
            uint8_t s   = (uint8_t)(((p + rr) % len) * seg / len);  /* 0..seg-1 */
            uint8_t hue = (uint8_t)(base + s * (uint8_t)(256U / seg));

            uint8_t hr, hg, hb;
            led_hsv_to_rgb(hue, &hr, &hg, &hb);
            struct led_rgb col = { .r = led_scale(hr, f->brightness),
                                   .g = led_scale(hg, f->brightness),
                                   .b = led_scale(hb, f->brightness) };
            led_ring_set(f, ring, p, col);
        }
    }

    uint16_t prev = rot;
    /* max_step 2, deliberately low: the wedges have hard edges, so a multi-pixel
     * jump reads as the pattern skipping rather than spinning. This keeps the
     * advance at 1 px/frame (framerate-limited, smooth) across the whole range
     * and only steps 2 at the very top (speed 255), so motion stays fluid. */
    rot = (uint16_t)((rot + led_speed_step(f->speed, 2U)) % len);
    if (rot < prev) {
        companion_hue += (uint8_t)(40U + (sys_rand32_get() % 176U));
        led_cycle_color(f);
    }

    return led_speed_delay(f->speed, 5U, 70U);
}

/*
 * Confetti — random pixels pop on in random vivid colours and fade out. Each
 * pop keeps its own hue as it decays (per-pixel brightness + hue). Ignores the
 * global colour entirely, like the rainbow, so it never calls led_cycle_color.
 */
static uint32_t render_confetti(const struct led_frame *f)
{
    static uint8_t c_bri[LED_MAX_PIXELS];
    static uint8_t c_hue[LED_MAX_PIXELS];

    /* Faster fade past the midpoint → a quicker turnover at the top of the
     * range despite the refresh floor; more pops keep it from thinning out. */
    uint16_t sp = led_speed_step(f->speed, 4U);       /* 1 → 4 across the top half */
    const uint8_t decay = (uint8_t)(12U * sp);        /* 12 → 48 */

    for (uint16_t i = 0; i < f->count; i++) {
        c_bri[i] = (c_bri[i] > decay) ? (uint8_t)(c_bri[i] - decay) : 0U;
    }

    /* pop a few new confetti each frame, denser at higher speed */
    uint8_t pops = (uint8_t)(1U + (f->speed >> 5));   /* 1..8 */
    for (uint8_t k = 0; k < pops; k++) {
        uint16_t i = (uint16_t)(sys_rand32_get() % f->count);
        c_bri[i] = 255U;
        c_hue[i] = (uint8_t)sys_rand32_get();
    }

    for (uint16_t i = 0; i < f->count; i++) {
        if (c_bri[i] != 0U) {
            uint8_t hr, hg, hb;
            led_hsv_to_rgb(c_hue[i], &hr, &hg, &hb);
            uint8_t bri = led_scale(c_bri[i], f->brightness);
            f->pixels[i] = (struct led_rgb){ .r = led_scale(hr, bri),
                                             .g = led_scale(hg, bri),
                                             .b = led_scale(hb, bri) };
        } else {
            f->pixels[i] = (struct led_rgb){ .r = 0, .g = 0, .b = 0 };
        }
    }

    return led_speed_delay(f->speed, 8U, 60U);
}

/* ── Dispatch ────────────────────────────────────────────────────────────── */

uint32_t led_render(const struct led_frame *f)
{
    switch (f->effect) {
    case EFFECT_OFF:       return render_off(f);
    case EFFECT_SOLID:     return render_solid(f);
    case EFFECT_RAINBOW:   return render_rainbow(f);
    case EFFECT_BREATHE:   return render_breathe(f);
    case EFFECT_WORM:      return render_worm(f);
    case EFFECT_SPARKLE:   return render_sparkle(f);
    case EFFECT_AURORA:    return render_aurora(f);
    case EFFECT_HEARTBEAT: return render_heartbeat(f);
    case EFFECT_FIREFLY:   return render_firefly(f);
    case EFFECT_PINWHEEL:  return render_pinwheel(f);
    case EFFECT_CONFETTI:  return render_confetti(f);
    default:               return render_off(f);
    }
}
