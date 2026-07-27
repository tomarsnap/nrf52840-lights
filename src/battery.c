#include "battery.h"
#include "led_effects.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>

#if defined(CONFIG_BT_BAS)
#include <zephyr/bluetooth/services/bas.h>
#endif

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#define VBATT_NODE  DT_NODELABEL(vbatt)

#if !DT_NODE_HAS_STATUS(VBATT_NODE, okay)

/*
 * No divider fitted — see the vbatt node in the board overlay. Everything
 * compiles to stubs so main() needs no conditionals; battery_init() fails
 * with -ENODEV, which it already treats as non-fatal.
 */
int battery_init(void)
{
    LOG_INF("No battery divider fitted (vbatt disabled) — monitoring off");
    return -ENODEV;
}

void battery_start(void) { }
int battery_millivolts(void) { return -ENODATA; }
uint8_t battery_percent(void) { return 0; }
bool battery_is_critical(void) { return false; }

#else

static const struct adc_dt_spec adc_ch = ADC_DT_SPEC_GET(VBATT_NODE);

/* Divider maths: the ADC sees output_ohms/full_ohms of the cell voltage. */
#define DIVIDER_FULL_OHMS    DT_PROP(VBATT_NODE, full_ohms)
#define DIVIDER_OUTPUT_OHMS  DT_PROP(VBATT_NODE, output_ohms)

BUILD_ASSERT(DIVIDER_OUTPUT_OHMS > 0, "vbatt output-ohms must be non-zero");
BUILD_ASSERT(DIVIDER_FULL_OHMS >= DIVIDER_OUTPUT_OHMS,
             "vbatt full-ohms must be >= output-ohms");

/*
 * Thresholds, at the cell.
 *
 * These are deliberately well above the INR18650-35E's 2.5 V absolute floor.
 * Two reasons: readings are taken under load and sag several hundred mV below
 * the resting voltage, and the drain continues after cutoff (see battery.h),
 * so the margin is what keeps the cell off the floor until the switch is
 * thrown. Below ~2.5 V a Li-ion cell forms copper shunts and can ignite on
 * the NEXT charge, not at the time — hence the conservative numbers.
 */
#define BATT_CRITICAL_MV  3100   /* blank the LEDs */
#define BATT_LOW_MV       3400   /* warn only */

/*
 * Plausibility window. Outside this the reading is treated as a broken sense
 * circuit, NOT as a flat cell, and the lockout is not armed.
 *
 * Nothing below ~2.5 V can be a live cell that is currently powering the
 * board — the regulator would have dropped out first — so such a reading
 * means an unwired, floating or failed divider. Blanking the lights because
 * the measurement path broke would be the wrong failure mode: a sensor we
 * cannot trust must not be allowed to act.
 *
 * The upper bound catches the same fault in the other direction; no Li-ion
 * cell reaches 4.35 V.
 */
#define BATT_IMPLAUSIBLE_LO_MV  2500
#define BATT_IMPLAUSIBLE_HI_MV  4350

/*
 * Consecutive critical samples required before blanking. One noisy reading
 * should not kill the output, and at a 10 s interval this still reacts in
 * well under a minute.
 */
#define BATT_CRITICAL_STREAK  3

#define SAMPLE_INTERVAL   K_SECONDS(10)
#define SAMPLES_PER_READ  4      /* averaged, to damp LED switching noise */

/*
 * Li-ion discharge curve, descending. Linear interpolation between points.
 *
 * A straight 3.0-4.2 V linear map is badly wrong for Li-ion: the curve is
 * flat from ~90% to ~20%, so a linear gauge sits near 50% for most of the
 * runtime then collapses.
 */
struct batt_point {
    uint16_t mv;
    uint8_t  pct;
};

static const struct batt_point curve[] = {
    { 4200, 100 }, { 4100, 90 }, { 4000, 80 }, { 3950, 70 },
    { 3870, 60 },  { 3820, 50 }, { 3790, 40 }, { 3750, 30 },
    { 3700, 20 },  { 3600, 10 }, { 3400,  5 }, { 3000,  0 },
};

static int32_t  last_mv  = -1;
static uint8_t  last_pct;
static bool     critical;
static K_MUTEX_DEFINE(batt_mutex);

static uint8_t mv_to_percent(int32_t mv)
{
    if (mv >= curve[0].mv) {
        return 100;
    }

    for (size_t i = 1; i < ARRAY_SIZE(curve); i++) {
        if (mv >= curve[i].mv) {
            const struct batt_point *hi = &curve[i - 1];
            const struct batt_point *lo = &curve[i];
            int32_t span = hi->mv - lo->mv;
            int32_t into = mv - lo->mv;

            return lo->pct + (uint8_t)((into * (hi->pct - lo->pct)) / span);
        }
    }

    return 0;
}

static int sample_once(int32_t *out_mv)
{
    int16_t raw;
    struct adc_sequence seq = {
        .buffer      = &raw,
        .buffer_size = sizeof(raw),
    };

    int err = adc_sequence_init_dt(&adc_ch, &seq);
    if (err) {
        return err;
    }

    err = adc_read(adc_ch.dev, &seq);
    if (err) {
        return err;
    }

    int32_t mv = raw;

    err = adc_raw_to_millivolts_dt(&adc_ch, &mv);
    if (err) {
        return err;
    }

    /* Scale back up through the divider to the cell voltage. */
    *out_mv = (mv * DIVIDER_FULL_OHMS) / DIVIDER_OUTPUT_OHMS;
    return 0;
}

static void battery_thread_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    int  critical_streak = 0;
    bool sense_fault     = false;

    while (1) {
        int32_t total = 0;
        int     good  = 0;

        for (int i = 0; i < SAMPLES_PER_READ; i++) {
            int32_t mv;

            if (sample_once(&mv) == 0) {
                total += mv;
                good++;
            }
            k_sleep(K_MSEC(5));
        }

        if (good == 0) {
            LOG_ERR("Battery sampling failed");
            k_sleep(SAMPLE_INTERVAL);
            continue;
        }

        int32_t mv = total / good;

        /* Sense circuit broken or not wired — report it and act on nothing. */
        if (mv < BATT_IMPLAUSIBLE_LO_MV || mv > BATT_IMPLAUSIBLE_HI_MV) {
            if (!sense_fault) {
                sense_fault = true;
                LOG_ERR("Battery sense implausible (%d mV) — divider missing "
                        "or broken. Monitoring disabled; LEDs unaffected.", mv);
            }
            critical_streak = 0;
            k_sleep(SAMPLE_INTERVAL);
            continue;
        }

        if (sense_fault) {
            sense_fault = false;
            LOG_INF("Battery sense recovered");
        }

        uint8_t pct = mv_to_percent(mv);

        k_mutex_lock(&batt_mutex, K_FOREVER);
        last_mv  = mv;
        last_pct = pct;
        bool already_critical = critical;
        k_mutex_unlock(&batt_mutex);

#if defined(CONFIG_BT_BAS)
        (void)bt_bas_set_battery_level(pct);
#endif

        if (mv <= BATT_CRITICAL_MV) {
            critical_streak++;
        } else {
            critical_streak = 0;
        }

        if (!already_critical && critical_streak >= BATT_CRITICAL_STREAK) {
            k_mutex_lock(&batt_mutex, K_FOREVER);
            critical = true;
            k_mutex_unlock(&batt_mutex);

            /*
             * Latched deliberately. Blanking the LEDs removes most of the
             * load, so the cell rebounds above the threshold within seconds;
             * an unlatched check would switch back on and oscillate.
             */
            LOG_ERR("Battery critical (%d mV) — blanking LEDs. "
                    "TURN THE POWER SWITCH OFF: this does not stop the drain.",
                    mv);
            led_effects_set_lockout(true);
        } else if (mv <= BATT_LOW_MV) {
            LOG_WRN("Battery low: %d mV (%u%%)", mv, pct);
        } else {
            LOG_INF("Battery: %d mV (%u%%)", mv, pct);
        }

        k_sleep(SAMPLE_INTERVAL);
    }
}

K_THREAD_DEFINE(battery_thread, 1024, battery_thread_fn, NULL, NULL, NULL,
                7, 0, K_TICKS_FOREVER);

int battery_init(void)
{
    if (!adc_is_ready_dt(&adc_ch)) {
        LOG_ERR("ADC channel not ready");
        return -ENODEV;
    }

    int err = adc_channel_setup_dt(&adc_ch);
    if (err) {
        LOG_ERR("ADC channel setup failed: %d", err);
        return err;
    }

    LOG_INF("Battery monitor ready (divider %u/%u)",
            DIVIDER_OUTPUT_OHMS, DIVIDER_FULL_OHMS);
    return 0;
}

void battery_start(void)
{
    k_thread_start(battery_thread);
}

int battery_millivolts(void)
{
    k_mutex_lock(&batt_mutex, K_FOREVER);
    int32_t mv = last_mv;
    k_mutex_unlock(&batt_mutex);

    return (mv < 0) ? -ENODATA : (int)mv;
}

uint8_t battery_percent(void)
{
    k_mutex_lock(&batt_mutex, K_FOREVER);
    uint8_t pct = last_pct;
    k_mutex_unlock(&batt_mutex);

    return pct;
}

bool battery_is_critical(void)
{
    k_mutex_lock(&batt_mutex, K_FOREVER);
    bool c = critical;
    k_mutex_unlock(&batt_mutex);

    return c;
}

#endif /* DT_NODE_HAS_STATUS(VBATT_NODE, okay) */
