/*
 * BL-LED — WS2812 ring controller, driven over BLE.
 *
 * Hardware notes that are easy to get wrong (see HANDOVER_NOTES.md):
 *   - The ring is driven over I2S, not SPI. SPI crashes this board.
 *   - Power the ring from an EXTERNAL supply with a shared ground. Powering
 *     it from RAW does not work: RAW reads ~4.24 V unloaded but collapses
 *     under load, and the ring stays dark.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fatal.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/logging/log.h>

#include <hal/nrf_gpio.h>

#include "ble_service.h"
#include "led_effects.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/*
 * Crash reporter on the onboard LED.
 *
 * A fatal error halts with interrupts disabled, and USB CDC needs interrupts
 * to transmit — so a crash dump can never reach the console on this board.
 * The LED is the only channel that survives. Pattern, repeating forever:
 *
 *   3 LONG blinks (600 ms) = "this is a crash report"
 *   then N SHORT blinks    = reason + 1:
 *       1 CPU exception  2 spurious IRQ  3 stack overflow
 *       4 kernel oops    5 kernel panic
 *      20 = reason 19 = K_ERR_ARM_MEM_DATA_ACCESS (MPU data-access fault)
 *
 * A SOLID (non-blinking) LED means it died before even this ran.
 */
#define FAULT_LED_PIN NRF_GPIO_PIN_MAP(0, 15)

void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf)
{
    ARG_UNUSED(esf);

    nrf_gpio_cfg_output(FAULT_LED_PIN);

    while (1) {
        for (int i = 0; i < 3; i++) {
            nrf_gpio_pin_write(FAULT_LED_PIN, 1);
            k_busy_wait(600000);
            nrf_gpio_pin_write(FAULT_LED_PIN, 0);
            k_busy_wait(300000);
        }

        k_busy_wait(1200000);

        for (unsigned int i = 0; i <= reason; i++) {
            nrf_gpio_pin_write(FAULT_LED_PIN, 1);
            k_busy_wait(150000);
            nrf_gpio_pin_write(FAULT_LED_PIN, 0);
            k_busy_wait(350000);
        }

        k_busy_wait(2500000);
    }
}

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

/*
 * Slow heartbeat on the onboard LED.
 *
 * Kept from the bring-up work on purpose: USB serial on this board drops
 * output silently and goes quiet on re-enumeration, so its silence proves
 * nothing. A blinking LED is the one signal that reliably distinguishes
 * "running" from "crashed" without a terminal attached.
 */
static void heartbeat_fn(void *a, void *b, void *c)
{
    ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

    if (!device_is_ready(led0.port)) {
        return;
    }

    gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);

    while (1) {
        gpio_pin_toggle_dt(&led0);
        k_sleep(K_MSEC(1000));
    }
}

K_THREAD_DEFINE(heartbeat_thread, 512, heartbeat_fn, NULL, NULL, NULL, 7, 0, 0);

int main(void)
{
    int err;

    /* USB CDC console. Deliberately does not wait for a terminal — the
     * device has to run headless. */
    (void)usb_enable(NULL);

    LOG_INF("BL-LED starting");

    err = led_effects_init();
    if (err) {
        LOG_ERR("LED strip init failed: %d", err);
    } else {
        led_effects_set_effect(EFFECT_RAINBOW);
        led_effects_set_brightness(64);
        led_effects_start();
    }

    /*
     * Let USB enumerate before starting Bluetooth.
     *
     * Removing this delay (while also changing the ring size) produced a
     * board that would not boot at all — solid LED, no USB. The last build
     * known to work had a long delay here, so BLE coming up before USB has
     * settled is a live suspect, independent of pixel count.
     */
    k_sleep(K_SECONDS(5));

    err = ble_service_init();
    if (err) {
        LOG_ERR("BLE start failed: %d — LEDs will still run locally", err);
    }

    return 0;
}
