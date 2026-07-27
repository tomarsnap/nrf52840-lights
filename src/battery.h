#pragma once

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery monitoring for a single Li-ion cell.
 *
 * SCOPE — read this before relying on it.
 *
 * This is a gauge and a convenience cutoff. It is NOT a protection circuit
 * and does not replace one:
 *
 *   - It only runs while the firmware runs. A crash, a hang, or a reflash
 *     leaves the cell entirely unguarded.
 *   - It cannot disconnect anything. Nothing in the battery's current path
 *     is under firmware control, so the "cutoff" only blanks the LEDs.
 *   - Blanking the LEDs does NOT stop the drain. A WS2812 draws ~1 mA even
 *     showing black, so a 72-pixel chain still pulls ~70 mA afterwards, plus
 *     whatever the SoC uses while advertising. The cell keeps discharging.
 *
 * The cutoff buys time and gives a visible warning. Only the hardware switch
 * actually removes the load.
 */

/* Set up the ADC channel. Returns 0, or a negative errno if the channel is
 * not ready. Monitoring does not start unless this succeeds. */
int battery_init(void);

/* Start the periodic sampling thread. */
void battery_start(void);

/* Last sampled cell voltage in millivolts, or -ENODATA before the first
 * valid sample. Readings outside a plausible cell range are treated as a
 * broken or unwired divider and discarded rather than acted on, so an
 * unpopulated sense circuit disables monitoring instead of blanking the
 * LEDs. */
int battery_millivolts(void);

/* Charge estimate 0-100 from the last sample, via a Li-ion discharge curve.
 * Readings taken under load sag, so this reads low while the LEDs are
 * bright — pessimistic, which is the safe direction. */
uint8_t battery_percent(void);

/* True once the critical threshold has been crossed. Latches: it does not
 * clear when the voltage recovers, because removing the LED load makes the
 * voltage jump back up and would otherwise oscillate. Cleared only by a
 * reboot. */
bool battery_is_critical(void);
