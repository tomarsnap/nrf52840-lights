#include "led_effects.h"
#include "led_animations.h"
#include "blled_ws2812.h"

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/dt-bindings/led/led.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/settings/settings.h>

LOG_MODULE_REGISTER(led_effects, LOG_LEVEL_INF);

#define STRIP_NODE        DT_ALIAS(led_strip)

/*
 * The DTS chain-length is the MAXIMUM number of pixels this build supports —
 * it sizes the pixel buffer and the driver's I2S DMA buffer, both of which
 * are static. The number actually lit is state_count, set at runtime.
 */
#define STRIP_MAX_PIXELS  DT_PROP(STRIP_NODE, chain_length)

/*
 * Brightness used until a value is restored from NVS. Deliberately low: a
 * first boot drives every pixel, and a full-brightness ring pulls ~60 mA per
 * pixel, which is more than most supplies (and USB) will give.
 */
#define DEFAULT_BRIGHTNESS  64

/*
 * Animation speed used until a value is restored from NVS. 128 = middle of the
 * 0-255 range, so effects animate at a moderate rate out of the box.
 */
#define DEFAULT_SPEED       128

static const struct device *const strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb pixels[STRIP_MAX_PIXELS];

/* State — written from BLE thread, read from LED thread */
static K_MUTEX_DEFINE(state_mutex);
static led_effect_t state_effect     = EFFECT_RAINBOW; /* boot default; overridden by settings */
static uint8_t      state_r          = 255;
static uint8_t      state_g          = 0;
static uint8_t      state_b          = 0;
static uint8_t      state_brightness = DEFAULT_BRIGHTNESS; /* overridden by settings */
static uint16_t     state_count      = STRIP_MAX_PIXELS;   /* overridden by settings */
static uint8_t      state_speed      = DEFAULT_SPEED;      /* overridden by settings */
static bool         state_auto_color;  /* re-randomise colour each cycle (persisted) */
static bool         state_lockout;   /* battery critical — force output black */
static int32_t      state_identify   = -1;  /* diagnostic single-pixel spotlight, -1 = off */
/* Strip colour order — default GRB (a stock WS2812); overridden by settings. */
static led_color_order_t state_color_order = LED_ORDER_GRB;

/*
 * Colour-order table: each order's on-wire channel list (one LED_COLOR_ID_* per
 * byte) and its stable protocol name. Indexed by led_color_order_t. The White
 * byte is emitted as 0 by the driver — struct led_rgb has no white channel — so
 * the *W orders drive an SK6812-RGBW strip's R/G/B correctly with white dark.
 */
static const struct color_order_def {
    uint8_t     mapping[4];
    uint8_t     num_colors;
    const char *name;
} color_orders[] = {
    [LED_ORDER_GRB]  = {{LED_COLOR_ID_GREEN, LED_COLOR_ID_RED,
                         LED_COLOR_ID_BLUE},                      3, "grb"},
    [LED_ORDER_RGB]  = {{LED_COLOR_ID_RED, LED_COLOR_ID_GREEN,
                         LED_COLOR_ID_BLUE},                      3, "rgb"},
    [LED_ORDER_GRBW] = {{LED_COLOR_ID_GREEN, LED_COLOR_ID_RED,
                         LED_COLOR_ID_BLUE, LED_COLOR_ID_WHITE},  4, "grbw"},
    [LED_ORDER_RGBW] = {{LED_COLOR_ID_RED, LED_COLOR_ID_GREEN,
                         LED_COLOR_ID_BLUE, LED_COLOR_ID_WHITE},  4, "rgbw"},
};

/*
 * Ring calibration — the physical-mounting half of the geometry (see
 * led_animations.h). base/size are derived from state_count at use time; only
 * top and dir are stored and persisted here.
 *
 * top = UINT16_MAX means "not calibrated": fall back to the ring's electrical
 * start. dir defaults to +1. Set via led_effects_set_ring_cal(), restored from
 * NVS at boot. Reset to the defaults in led_effects_init() before load.
 */
#define RING_TOP_UNSET  UINT16_MAX
static uint16_t     ring_top[LED_RINGS];
static int8_t       ring_dir[LED_RINGS];

/* Level the "identify" diagnostic lights its single pixel at — fixed and low so
 * it is always visible regardless of the saved brightness. */
#define IDENTIFY_LEVEL  64

/* ── Persistence ─────────────────────────────────────────────────────────── */
/*
 * Persisted under "led/" in NVS via the settings subsystem:
 *   led/count  uint16_t  active pixel count
 *   led/bri    uint8_t   brightness
 *   led/spd    uint8_t   animation speed
 *   led/auto   uint8_t   auto colour-cycle mode (0/1)
 *   led/order  uint8_t   strip colour order (led_color_order_t)
 *   led/fx     uint8_t   active effect (led_effect_t index)
 *   led/geo    blob      ring calibration (per-ring top index + direction)
 *
 * Saving is deferred to a work item rather than done inline: settings_save_one()
 * erases and writes flash, which blocks for milliseconds, and the setters are
 * called from the BLE RX callback. The delay also debounces an app that sends a
 * stream of values as the user drags a slider.
 *
 * Every field is written on each save. That is not wasteful: NVS compares
 * against the stored value and skips the write when unchanged, so an
 * unmodified field costs a read, not a flash erase.
 *
 * The effect IS persisted (led/fx) as a bare led_effect_t index. This is safe
 * only because the enum values are explicit and append-only — never renumber an
 * existing effect, or a saved index would restore the wrong one. On load the
 * value is range-checked against EFFECT_MAX, so a value written by a firmware
 * with more effects is ignored rather than restoring garbage.
 *
 * The colour is still deliberately NOT persisted. Add it here if that becomes
 * worth handling.
 */
#define SETTINGS_SAVE_DELAY  K_MSEC(750)

/* On-flash layout of the ring calibration blob ("led/geo"). */
struct geo_nv {
    uint16_t top[LED_RINGS];
    int8_t   dir[LED_RINGS];
};

static void settings_save_fn(struct k_work *work)
{
    ARG_UNUSED(work);

    struct geo_nv geo;

    k_mutex_lock(&state_mutex, K_FOREVER);
    uint16_t count      = state_count;
    uint8_t  brightness = state_brightness;
    uint8_t  speed      = state_speed;
    uint8_t  auto_color = state_auto_color ? 1U : 0U;
    uint8_t  order      = (uint8_t)state_color_order;
    uint8_t  effect     = (uint8_t)state_effect;
    for (int i = 0; i < LED_RINGS; i++) {
        geo.top[i] = ring_top[i];
        geo.dir[i] = ring_dir[i];
    }
    k_mutex_unlock(&state_mutex);

    int err = settings_save_one("led/count", &count, sizeof(count));
    if (err) {
        LOG_ERR("Failed to persist pixel count: %d", err);
    }

    err = settings_save_one("led/bri", &brightness, sizeof(brightness));
    if (err) {
        LOG_ERR("Failed to persist brightness: %d", err);
    }

    err = settings_save_one("led/spd", &speed, sizeof(speed));
    if (err) {
        LOG_ERR("Failed to persist speed: %d", err);
    }

    err = settings_save_one("led/auto", &auto_color, sizeof(auto_color));
    if (err) {
        LOG_ERR("Failed to persist auto-colour mode: %d", err);
    }

    err = settings_save_one("led/order", &order, sizeof(order));
    if (err) {
        LOG_ERR("Failed to persist colour order: %d", err);
    }

    err = settings_save_one("led/fx", &effect, sizeof(effect));
    if (err) {
        LOG_ERR("Failed to persist effect: %d", err);
    }

    err = settings_save_one("led/geo", &geo, sizeof(geo));
    if (err) {
        LOG_ERR("Failed to persist ring calibration: %d", err);
    }

    LOG_INF("Settings saved: %u pixels, brightness %u, speed %u",
            count, brightness, speed);
}

static K_WORK_DELAYABLE_DEFINE(settings_save_work, settings_save_fn);

static int led_settings_set(const char *name, size_t len,
                            settings_read_cb read_cb, void *cb_arg)
{
    const char *next;

    if (settings_name_steq(name, "count", &next) && !next) {
        uint16_t count;

        if (len != sizeof(count)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &count, sizeof(count));
        if (rc < 0) {
            return (int)rc;
        }

        /* Guard against a value saved by a build with a larger chain. */
        if (count == 0U || count > STRIP_MAX_PIXELS) {
            LOG_WRN("Stored count %u out of range (1-%d), ignoring",
                    count, STRIP_MAX_PIXELS);
            return 0;
        }

        state_count = count;
        LOG_INF("Restored pixel count: %u", count);
        return 0;
    }

    if (settings_name_steq(name, "bri", &next) && !next) {
        uint8_t brightness;

        if (len != sizeof(brightness)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &brightness, sizeof(brightness));
        if (rc < 0) {
            return (int)rc;
        }

        /* Any uint8_t is a valid brightness, so no range check needed. */
        state_brightness = brightness;
        LOG_INF("Restored brightness: %u", brightness);
        return 0;
    }

    if (settings_name_steq(name, "spd", &next) && !next) {
        uint8_t speed;

        if (len != sizeof(speed)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &speed, sizeof(speed));
        if (rc < 0) {
            return (int)rc;
        }

        /* Any uint8_t is a valid speed, so no range check needed. */
        state_speed = speed;
        LOG_INF("Restored speed: %u", speed);
        return 0;
    }

    if (settings_name_steq(name, "auto", &next) && !next) {
        uint8_t auto_color;

        if (len != sizeof(auto_color)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &auto_color, sizeof(auto_color));
        if (rc < 0) {
            return (int)rc;
        }

        state_auto_color = (auto_color != 0U);
        LOG_INF("Restored auto-colour mode: %u", auto_color);
        return 0;
    }

    if (settings_name_steq(name, "order", &next) && !next) {
        uint8_t order;

        if (len != sizeof(order)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &order, sizeof(order));
        if (rc < 0) {
            return (int)rc;
        }

        /* Guard against a value saved by a firmware with more orders. */
        if (order > LED_ORDER_MAX) {
            LOG_WRN("Stored colour order %u out of range, ignoring", order);
            return 0;
        }

        state_color_order = (led_color_order_t)order;
        LOG_INF("Restored colour order: %s", color_orders[order].name);
        return 0;
    }

    if (settings_name_steq(name, "fx", &next) && !next) {
        uint8_t effect;

        if (len != sizeof(effect)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &effect, sizeof(effect));
        if (rc < 0) {
            return (int)rc;
        }

        /* Guard against a value saved by a firmware with more effects. */
        if (effect > EFFECT_MAX) {
            LOG_WRN("Stored effect %u out of range, ignoring", effect);
            return 0;
        }

        state_effect = (led_effect_t)effect;
        LOG_INF("Restored effect: %u", effect);
        return 0;
    }

    if (settings_name_steq(name, "geo", &next) && !next) {
        struct geo_nv geo;

        if (len != sizeof(geo)) {
            return -EINVAL;
        }

        ssize_t rc = read_cb(cb_arg, &geo, sizeof(geo));
        if (rc < 0) {
            return (int)rc;
        }

        /* Range of each top is re-validated at use time against the current
         * ring size, so store as-is; only sanitise the direction. */
        for (int i = 0; i < LED_RINGS; i++) {
            ring_top[i] = geo.top[i];
            ring_dir[i] = (geo.dir[i] < 0) ? -1 : 1;
        }
        LOG_INF("Restored ring calibration");
        return 0;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(led_effects, "led", NULL,
                               led_settings_set, NULL, NULL);

/* ── Strip plumbing ──────────────────────────────────────────────────────── */

/*
 * NOTE: do not add an i2s_trigger(DROP) here.
 *
 * It was tried, to force the peripheral out of I2S_STATE_STOPPING between
 * frames, and it made things strictly worse: DROP calls nrfx_i2s_stop() but
 * relies on the completion handler to finish teardown, so the next START
 * failed with nrfx INVALID_STATE (0x0bad000c) and NOTHING was sent after the
 * first frame. Without it, some later frames do get through.
 */
/*
 * Always transmits a FULL chain-length frame, blanking any pixels past the
 * active count.
 *
 * Passing a short count to led_strip_update_rgb() looks like the obvious way
 * to drive a shorter chain, but it is wrong: the driver serialises only the
 * pixels given, appends the reset gap, then hands i2s_write() the full
 * tx_buf_bytes regardless. The reset would land mid-buffer and the leftover
 * tail — uninitialised slab memory — would clock out behind it as a second,
 * garbage frame. Sending the whole chain keeps exactly one frame on the wire;
 * the surplus falls off the end of a shorter strip harmlessly.
 */
static uint16_t active_count(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    uint16_t count = state_count;
    k_mutex_unlock(&state_mutex);

    return count;
}

/*
 * Split `total` active pixels into equal rings. Returns the ring count and
 * writes the per-ring size. If the total does not divide evenly across
 * LED_RINGS (or is zero), it degrades to a single ring spanning the whole
 * strip, so ring-based effects still render something sane. Pure — no locking.
 */
static uint8_t geometry_split(uint16_t total, uint16_t *ring_size)
{
    uint8_t rc = LED_RINGS;

    if (rc == 0U || total == 0U || (total % rc) != 0U) {
        rc = 1U;
    }
    *ring_size = total / rc;
    return rc;
}

/*
 * `count` is snapshotted once per frame by the caller so the loop that fills
 * the pixels and the blanking below cannot disagree if BLE changes it midway.
 */
static int strip_flush(uint16_t count)
{
    if (count < STRIP_MAX_PIXELS) {
        memset(&pixels[count], 0,
               (STRIP_MAX_PIXELS - count) * sizeof(pixels[0]));
    }

    return led_strip_update_rgb(strip, pixels, STRIP_MAX_PIXELS);
}

static int fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    uint16_t count = active_count();

    for (uint16_t i = 0; i < count; i++) {
        pixels[i].r = led_scale(r, bri);
        pixels[i].g = led_scale(g, bri);
        pixels[i].b = led_scale(b, bri);
    }
    return strip_flush(count);
}

/* ── LED thread ──────────────────────────────────────────────────────────── */

static void led_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    while (1) {
        /* Snapshot the live state under the lock, then release it: the render
         * below must not hold the mutex (it can run for a whole frame) and an
         * effect only ever sees one consistent, race-free view of the state. */
        k_mutex_lock(&state_mutex, K_FOREVER);
        struct led_frame frame = {
            .pixels     = pixels,
            .count      = state_count,
            .effect     = state_effect,
            .r          = state_r,
            .g          = state_g,
            .b          = state_b,
            .brightness = state_brightness,
            .speed      = state_speed,
            .auto_color = state_auto_color,
        };
        bool    lockout  = state_lockout;
        int32_t identify = state_identify;

        /* Resolve the ring geometry for this frame: derive base/size from the
         * active count, then apply each ring's calibrated top and direction
         * (falling back to the electrical start if uncalibrated). */
        uint16_t ring_size;
        uint8_t  rc = geometry_split(frame.count, &ring_size);
        frame.ring_count = rc;
        for (uint8_t i = 0; i < rc; i++) {
            uint16_t base = (uint16_t)(i * ring_size);
            uint16_t top  = ring_top[i];

            if (top < base || top >= base + ring_size) {
                top = base;   /* uncalibrated or stale after a count change */
            }
            frame.rings[i] = (struct led_ring){
                .base = base,
                .size = ring_size,
                .top  = top,
                .dir  = (ring_dir[i] < 0) ? -1 : 1,
            };
        }
        k_mutex_unlock(&state_mutex);

        /*
         * Battery critical. Hold the strip black and ignore the effect.
         *
         * This saves the LED current but NOT the WS2812 quiescent draw
         * (~1 mA per pixel even when black), so the cell still discharges.
         * Only the hardware switch stops that.
         */
        if (lockout) {
            memset(pixels, 0, sizeof(pixels));
            strip_flush(frame.count);
            k_sleep(K_MSEC(500));
            continue;
        }

        /*
         * Identify diagnostic: light exactly one physical pixel so a ring's top
         * can be located, ignoring the effect until a new one is selected.
         */
        if (identify >= 0 && identify < STRIP_MAX_PIXELS) {
            memset(pixels, 0, sizeof(pixels));
            pixels[identify].r = IDENTIFY_LEVEL;
            pixels[identify].g = IDENTIFY_LEVEL;
            pixels[identify].b = IDENTIFY_LEVEL;
            strip_flush(frame.count);
            k_sleep(K_MSEC(200));
            continue;
        }

        /* All the actual animation lives in led_animations.c. It fills the
         * active pixels and tells us how long to wait for the next frame; we
         * push the buffer to the strip (blanking any pixels past the active
         * count — see strip_flush) and sleep. */
        uint32_t delay_ms = led_render(&frame);

        strip_flush(frame.count);
        k_sleep(K_MSEC(delay_ms));
    }
}

/* K_TICKS_FOREVER: do not auto-start. led_effects_start() starts the thread
 * once the caller decides driving the whole strip is safe. */
K_THREAD_DEFINE(led_thread, 2048, led_thread_fn, NULL, NULL, NULL, 7, 0,
                K_TICKS_FOREVER);

/* ── Colour order ─────────────────────────────────────────────────────────── */

/*
 * Push a colour order to the strip driver. The driver makes the change on its
 * next frame and serialises accesses with its own lock, so this is safe to call
 * from the BLE thread while the LED thread is rendering. Pure re-programming —
 * no persistence; the caller schedules the flash save.
 */
static void apply_color_order(led_color_order_t order)
{
    if ((int)order < 0 || order > LED_ORDER_MAX) {
        return;
    }

    const struct color_order_def *d = &color_orders[order];
    int err = blled_ws2812_set_color_order(strip, d->mapping, d->num_colors);

    if (err) {
        LOG_ERR("Failed to set colour order %s: %d", d->name, err);
    } else {
        LOG_INF("Colour order → %s", d->name);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int led_effects_init(void)
{
    if (!device_is_ready(strip)) {
        LOG_ERR("LED strip not ready: %s", strip->name);
        return -ENODEV;
    }

    /* Ring calibration defaults, applied before load so a stored blob overrides
     * them and an absent one leaves every ring uncalibrated (top = electrical
     * start, dir = +1). */
    for (int i = 0; i < LED_RINGS; i++) {
        ring_top[i] = RING_TOP_UNSET;
        ring_dir[i] = 1;
    }

    /* Restore the persisted pixel count. A failure here is not fatal — the
     * strip still runs, just at the compile-time maximum. */
    int err = settings_subsys_init();
    if (err) {
        LOG_ERR("settings_subsys_init failed: %d", err);
    } else {
        err = settings_load();
        if (err) {
            LOG_ERR("settings_load failed: %d", err);
        }
    }

    /* Program the restored (or default) colour order into the strip driver
     * before the first frame is rendered, so pixels come out in the right
     * order from the very first update. */
    apply_color_order(state_color_order);

    LOG_INF("LED strip ready: %u active of %d max pixels, brightness %u, order %s",
            state_count, STRIP_MAX_PIXELS, state_brightness,
            color_orders[state_color_order].name);
    return 0;
}

void led_effects_start(void)
{
    k_thread_start(led_thread);
}

int led_effects_pixel_count(void)
{
    return active_count();
}

int led_effects_max_pixels(void)
{
    return STRIP_MAX_PIXELS;
}

int led_effects_set_count(uint16_t count)
{
    if (count == 0U || count > STRIP_MAX_PIXELS) {
        return -EINVAL;
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    state_count = count;
    k_mutex_unlock(&state_mutex);

    /* Reschedules if already pending, so a burst of changes writes once. */
    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    LOG_INF("Pixel count → %u", count);
    return 0;
}

int led_effects_direct_fill(uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    return fill(r, g, b, bri);
}

int led_effects_direct_pixel(int idx, uint8_t r, uint8_t g, uint8_t b, uint8_t bri)
{
    uint16_t count = active_count();

    if (idx < 0 || idx >= count) {
        return -EINVAL;
    }

    memset(pixels, 0, sizeof(pixels));
    pixels[idx].r = led_scale(r, bri);
    pixels[idx].g = led_scale(g, bri);
    pixels[idx].b = led_scale(b, bri);

    return strip_flush(count);
}

void led_effects_set_effect(led_effect_t effect)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_effect   = effect;
    state_identify = -1;   /* selecting an effect leaves identify mode */
    k_mutex_unlock(&state_mutex);
    LOG_INF("Effect → %d", effect);

    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);
}

led_effect_t led_effects_get_effect(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    led_effect_t effect = state_effect;
    k_mutex_unlock(&state_mutex);

    return effect;
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

void led_effects_get_color(uint8_t *r, uint8_t *g, uint8_t *b)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    *r = state_r;
    *g = state_g;
    *b = state_b;
    k_mutex_unlock(&state_mutex);
}

void led_effects_set_brightness(uint8_t brightness)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_brightness = brightness;
    k_mutex_unlock(&state_mutex);

    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    LOG_INF("Brightness → %d", brightness);
}

void led_effects_set_speed(uint8_t speed)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_speed = speed;
    k_mutex_unlock(&state_mutex);

    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    LOG_INF("Speed → %d", speed);
}

uint8_t led_effects_get_speed(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    uint8_t speed = state_speed;
    k_mutex_unlock(&state_mutex);

    return speed;
}

void led_effects_set_auto_color(bool on)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_auto_color = on;
    k_mutex_unlock(&state_mutex);

    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    LOG_INF("Auto colour cycle → %s", on ? "on" : "off");
}

bool led_effects_get_auto_color(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    bool on = state_auto_color;
    k_mutex_unlock(&state_mutex);

    return on;
}

int led_effects_set_color_order(led_color_order_t order)
{
    if ((int)order < 0 || order > LED_ORDER_MAX) {
        return -EINVAL;
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    state_color_order = order;
    k_mutex_unlock(&state_mutex);

    /* Re-program the strip now; persist after the usual debounce. */
    apply_color_order(order);
    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    return 0;
}

led_color_order_t led_effects_get_color_order(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    led_color_order_t order = state_color_order;
    k_mutex_unlock(&state_mutex);

    return order;
}

const char *led_color_order_name(led_color_order_t order)
{
    if ((int)order < 0 || order > LED_ORDER_MAX) {
        return "?";
    }

    return color_orders[order].name;
}

void led_effects_cycle_color(void)
{
    /* Remember the last hue so each jump is by a random but non-trivial amount:
     * a full spin of the wheel minus a slice, so consecutive colours are always
     * clearly different (never two near-identical reds in a row) yet still
     * unpredictable. Runs on the effect thread, one call per cycle. */
    static uint8_t hue;

    hue += (uint8_t)(40U + (sys_rand32_get() % 176U));   /* +40..215 */

    uint8_t r, g, b;
    led_hsv_to_rgb(hue, &r, &g, &b);

    k_mutex_lock(&state_mutex, K_FOREVER);
    state_r = r;
    state_g = g;
    state_b = b;
    k_mutex_unlock(&state_mutex);
}

/* ── Ring geometry ─────────────────────────────────────────────────────────── */

int led_effects_ring_count(void)
{
    uint16_t ring_size;
    return geometry_split(active_count(), &ring_size);
}

int led_effects_ring_size(void)
{
    uint16_t ring_size;
    (void)geometry_split(active_count(), &ring_size);
    return ring_size;
}

int led_effects_ring_top(int ring)
{
    uint16_t ring_size;
    uint8_t  rc = geometry_split(active_count(), &ring_size);

    if (ring < 0 || ring >= rc) {
        return -1;
    }

    uint16_t base = (uint16_t)(ring * ring_size);

    k_mutex_lock(&state_mutex, K_FOREVER);
    uint16_t top = ring_top[ring];
    k_mutex_unlock(&state_mutex);

    return (top < base || top >= base + ring_size) ? base : top;
}

int led_effects_ring_dir(int ring)
{
    if (ring < 0 || ring >= led_effects_ring_count()) {
        return 0;
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    int8_t dir = ring_dir[ring];
    k_mutex_unlock(&state_mutex);

    return (dir < 0) ? -1 : 1;
}

int led_effects_set_ring_cal(int ring, int top, int dir)
{
    uint16_t ring_size;
    uint8_t  rc = geometry_split(active_count(), &ring_size);

    if (ring < 0 || ring >= rc) {
        return -EINVAL;
    }
    if (dir != 1 && dir != -1) {
        return -EINVAL;
    }

    uint16_t base = (uint16_t)(ring * ring_size);
    if (top < base || top >= base + ring_size) {
        return -EINVAL;   /* top must be a physical pixel within this ring */
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    ring_top[ring] = (uint16_t)top;
    ring_dir[ring] = (int8_t)dir;
    k_mutex_unlock(&state_mutex);

    k_work_reschedule(&settings_save_work, SETTINGS_SAVE_DELAY);

    LOG_INF("Ring %d calibrated: top %d dir %+d", ring, top, dir);
    return 0;
}

void led_effects_identify(int pixel)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_identify = (pixel >= 0 && pixel < STRIP_MAX_PIXELS) ? pixel : -1;
    k_mutex_unlock(&state_mutex);

    if (pixel >= 0) {
        LOG_INF("Identify → pixel %d", pixel);
    } else {
        LOG_INF("Identify off");
    }
}

void led_effects_set_lockout(bool lockout)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    state_lockout = lockout;
    k_mutex_unlock(&state_mutex);

    LOG_WRN("LED output %s", lockout ? "LOCKED OUT (battery)" : "released");
}

bool led_effects_is_locked_out(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    bool lockout = state_lockout;
    k_mutex_unlock(&state_mutex);

    return lockout;
}

uint8_t led_effects_get_brightness(void)
{
    k_mutex_lock(&state_mutex, K_FOREVER);
    uint8_t brightness = state_brightness;
    k_mutex_unlock(&state_mutex);

    return brightness;
}
