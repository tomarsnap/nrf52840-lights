#pragma once

#include <stdint.h>
#include <zephyr/device.h>

/*
 * Runtime colour-order control for the in-tree "blled,ws2812-i2s-runtime"
 * driver (drivers/ws2812_i2s_runtime/).
 *
 * The stock Zephyr ws2812-i2s driver freezes the on-wire byte order AND the
 * channel count at build time (its color_mapping is `static const` and the I2S
 * DMA buffer is sized from the mapping length). This fork moves the mapping and
 * count into mutable driver state so one firmware image can drive both a
 * 3-channel GRB WS2812 strip and a 4-channel GRBW SK6812-RGBW strip, chosen at
 * run time and persisted to flash by the app (see led_effects.c).
 *
 * Set the order the strip expects. `mapping` lists one LED_COLOR_ID_* per wire
 * byte, in on-wire order (e.g. {GREEN, RED, BLUE} for GRB, {GREEN, RED, BLUE,
 * WHITE} for GRBW). `num_colors` is how many entries `mapping` has; it must not
 * exceed the node's `max-colors` (which sized the DMA buffer). The White channel
 * is emitted as 0 — struct led_rgb carries no white — so RGBW strips show
 * correct R/G/B with the white die dark.
 *
 * Thread-safe: takes the driver's lock, so it is safe to call from a different
 * thread than the one driving the strip. Takes effect on the next frame.
 *
 * Returns 0 on success, -EINVAL if num_colors is 0 or above max-colors or a
 * mapping entry is not a RED/GREEN/BLUE/WHITE colour id, -ENODEV if dev is not
 * this driver.
 */
int blled_ws2812_set_color_order(const struct device *dev,
				 const uint8_t *mapping, uint8_t num_colors);
