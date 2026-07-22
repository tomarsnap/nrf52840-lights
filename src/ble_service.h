#pragma once

/**
 * Initialise Bluetooth and start advertising the NUS LED controller.
 * Returns 0 on success, negative errno on failure.
 */
int ble_service_init(void);
