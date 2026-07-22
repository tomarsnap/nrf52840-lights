#include "led_effects.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(led_effects, LOG_LEVEL_INF);

#define STRIP_NODE        DT_ALIAS(led_strip)
#define STRIP_NUM_PIXELS  DT_PROP(STRIP_NODE, chain_length)

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_NUM_PIXELS];

/* State — written from BLE thread, read from LED thread */
static K_MUTEX_DEFINE(state_mutex);
static led_effect_t state_effect     = EFFECT_RAINBOW; /* visible on boot for hw test */
static uint8_t      state_r          = 255;
static uint8_t      state_g          = 0;
static uint8_t      state_b          = 0;
static uint8_t      state_brightness = 128;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint8_t scale(uint8_t val, uint8_t brightness)
{
    return (uint16_t)val * brightness / 255U;
}

/*
 * NOTE: do not add an i2s_trigger(DROP) here.
 *
 * It was tried, to force the peripheral out of I2S_STATE_STOPPING between
 * frames, and it made things strictly worse: DROP calls nrfx_i2s_stop() but
 * relies on the completion handler to finish teardown, so the next START
 * failed with nrfx INVALID_STATE (0x0bad000c) and NOTHING was sent after the
 * first frame. Without it, some later frames do get through.
 */
static int strip_flush(void)
{
    return led_strip_update_rgb(strip, pixels, STRIP_NUM_PIXELS);
}

static int fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r = scale(r, bri);
        pixels[i].g = scale(g, bri);
        pixels[i].b = scale(b, bri);
    }
    return strip_flush();
}

/* Simple HSV→RGB: hue 0–255, sat=val=255 */
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

/* ── LED thread ──────────────────────────────────────────────────────────── */

static void led_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    uint8_t  rainbow_offset = 0;
    uint16_t breathe_step   = 0;

    while (1) {
        k_mutex_lock(&state_mutex, K_FOREVER);
        led_effect_t effect    = state_effect;
        uint8_t      r         = state_r;
        uint8_t      g         = state_g;
        uint8_t      b         = state_b;
        uint8_t      brightness = state_brightness;
        k_mutex_unlock(&state_mutex);

        switch (effect) {
        case EFFECT_OFF:
            fill(0, 0, 0, 0);
            k_sleep(K_MSEC(100));
            break;

        case EFFECT_SOLID:
            fill(r, g, b, brightness);
            k_sleep(K_MSEC(100));
            break;

        case EFFECT_RAINBOW:
            for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
                uint8_t hue = rainbow_offset + (uint8_t)(i * 256U / STRIP_NUM_PIXELS);
                uint8_t hr, hg, hb;
                hsv_to_rgb(hue, &hr, &hg, &hb);
                pixels[i].r = scale(hr, brightness);
                pixels[i].g = scale(hg, brightness);
                pixels[i].b = scale(hb, brightness);
            }
            strip_flush();
            rainbow_offset++;
            k_sleep(K_MSEC(30));
            break;

        case EFFECT_BREATHE: {
            /* triangle wave 0→254→0 over 256 steps */
            uint8_t wave = (breathe_step < 128U)
                         ? (uint8_t)(breathe_step * 2U)
                         : (uint8_t)((255U - breathe_step) * 2U);
            breathe_step = (breathe_step + 1U) & 0xFFU;

            uint8_t effective_bri = scale(wave, brightness);
            fill(r, g, b, effective_bri);
            k_sleep(K_MSEC(20));
            break;
        }
        }
    }
}

/* K_TICKS_FOREVER: do not auto-start. led_effects_start() starts the thread
 * once the caller decides driving the whole strip is safe. */
K_THREAD_DEFINE(led_thread, 2048, led_thread_fn, NULL, NULL, NULL, 7, 0,
                K_TICKS_FOREVER);

/* ── Public API ──────────────────────────────────────────────────────────── */

int led_effects_init(void)
{
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip not ready: %s", strip->name);
        return -ENODEV;
    }

    LOG_INF("LED strip ready: %d pixels", STRIP_NUM_PIXELS);
    return 0;
}

void led_effects_start(void)
{
    k_thread_start(led_thread);
}

int led_effects_pixel_count(void)
{
    return STRIP_NUM_PIXELS;
}

int led_effects_direct_fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    return fill(r, g, b, bri);
}

int led_effects_direct_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    if (idx < 0 || idx >= STRIP_NUM_PIXELS) {
        return -EINVAL;
    }

    memset(pixels, 0, sizeof(pixels));
    pixels[idx].r = scale(r, bri);
    pixels[idx].g = scale(g, bri);
    pixels[idx].b = scale(b, bri);

    return strip_flush();
}

void led_effects_set_effect(led_effect_t effect)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_effect = effect;
    k_mutex_unlock(&state_mutex);
    LOG_INF("Effect → %d", effect);
}

void led_effects_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_r = r;
    state_g = g;
    state_b = b;
    k_mutex_unlock(&state_mutex);
    LOG_INF("Color → %d,%d,%d", r, g, b);
}

void led_effects_set_brightness(uint8_t brightness)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_brightness = brightness;
    k_mutex_unlock(&state_mutex);
    LOG_INF("Brightness → %d", brightness);
}
