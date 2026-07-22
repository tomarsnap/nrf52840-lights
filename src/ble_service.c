#include "ble_service.h"
#include "led_effects.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>

#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(ble_service, LOG_LEVEL_INF);

/* ── Advertising data ────────────────────────────────────────────────────── */

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA(BT_DATA_NAME_COMPLETE,
            CONFIG_BT_DEVICE_NAME,
            sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int start_advertising(void)
{
    return bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), NULL, 0);
}

/* ── Connection callbacks ────────────────────────────────────────────────── */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }
    LOG_INF("BLE connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("BLE disconnected (reason %u) — restarting advertising", reason);
    start_advertising();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected    = on_connected,
    .disconnected = on_disconnected,
};

/* ── NUS receive handler ─────────────────────────────────────────────────── */
/*
 * Command protocol (ASCII, case-insensitive):
 *   E<0-3>          set effect  (0=off, 1=solid, 2=rainbow, 3=breathe)
 *   C<r>,<g>,<b>    set color   (0-255 per channel)
 *   B<0-255>        set brightness
 */
static void on_nus_received(struct bt_conn *conn,
                            const uint8_t  *data,
                            uint16_t        len)
{
    /* Copy to a null-terminated buffer; truncate if oversized */
    char buf[64];
    if (len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';

    /* Strip trailing newline / carriage-return */
    for (int i = len - 1; i >= 0 && (buf[i] == '\n' || buf[i] == '\r'); i--) {
        buf[i] = '\0';
    }

    LOG_INF("BLE cmd: \"%s\"", buf);

    char cmd = buf[0] | 0x20; /* to lowercase */

    switch (cmd) {
    case 'e': {
        int effect = atoi(&buf[1]);
        if (effect >= EFFECT_OFF && effect <= EFFECT_BREATHE) {
            led_effects_set_effect((led_effect_t)effect);
        } else {
            LOG_WRN("Unknown effect %d", effect);
        }
        break;
    }
    case 'c': {
        int r, g, b;
        if (sscanf(&buf[1], "%d,%d,%d", &r, &g, &b) == 3) {
            led_effects_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
        } else {
            LOG_WRN("Bad color format: %s", buf);
        }
        break;
    }
    case 'b': {
        int bri = atoi(&buf[1]);
        led_effects_set_brightness((uint8_t)bri);
        break;
    }
    default:
        LOG_WRN("Unknown command: %s", buf);
        break;
    }
}

static struct bt_nus_cb nus_cb = {
    .received = on_nus_received,
};

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * Called by the stack once the controller is up.
 *
 * bt_enable() is invoked ASYNCHRONOUSLY (with this callback) rather than
 * synchronously (with NULL). The synchronous form blocks the calling thread
 * until the controller is ready, and on this board it never returned: the
 * whole app appeared dead, USB never finished enumerating (enumeration needs
 * the workqueue, which was starved), and the heartbeat LED sat solid.
 *
 * Async means a stuck controller can no longer take the LEDs and console
 * down with it — the ring keeps running and this callback simply never fires.
 */
static void on_bt_ready(int err)
{
    if (err) {
        LOG_ERR("Bluetooth init failed: %d", err);
        return;
    }

    LOG_INF("Bluetooth controller ready");

    err = bt_nus_init(&nus_cb);
    if (err) {
        LOG_ERR("bt_nus_init failed: %d", err);
        return;
    }

    err = start_advertising();
    if (err) {
        LOG_ERR("Advertising start failed: %d", err);
        return;
    }

    LOG_INF("BLE ready — advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

int ble_service_init(void)
{
    int err = bt_enable(on_bt_ready);

    if (err) {
        LOG_ERR("bt_enable failed to start: %d", err);
        return err;
    }

    LOG_INF("Bluetooth starting (async)");
    return 0;
}
