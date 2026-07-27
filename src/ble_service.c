#include "battery.h"
#include "ble_service.h"
#include "led_effects.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <bluetooth/services/nus.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
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
 *   N<count>        set pixel count (1..chain-length), persisted to flash
 *   ?               report battery, pixel count and brightness
 */
/*
 * Every numeric argument is range-checked BEFORE being narrowed to uint8_t.
 *
 * Casting first is silently destructive: "B256" — an entirely reasonable thing
 * to type if you assume the range is 0-256 — wraps to 0, blanks the ring, and
 * persists that, so the next boot comes up dark and looks like dead hardware.
 * Negative values wrap just as badly ("B-5" becomes 251).
 */
static bool in_byte_range(int v)
{
    return v >= 0 && v <= 255;
}

/*
 * Send a reply over NUS, split into MTU-sized chunks.
 *
 * A single notification carries at most (ATT_MTU - 3) bytes — only 20 with the
 * default 23-byte MTU, which this peripheral never raises. The host stack
 * silently DROPS a longer notification (bt_att_create_pdu() returns NULL and
 * bt_gatt_notify() returns -ENOMEM), so a reply over 20 bytes just vanishes.
 * That is exactly why the short "sampling" reply used to arrive but the full
 * status line did not. Chunking works at whatever MTU is negotiated; a terminal
 * reassembles the pieces into one line.
 */
static void nus_send_all(struct bt_conn *conn, const char *data, uint16_t len)
{
    uint16_t mtu   = bt_gatt_get_mtu(conn);
    uint16_t chunk = (mtu > 3U) ? (uint16_t)(mtu - 3U) : 20U;

    for (uint16_t off = 0; off < len; off += chunk) {
        uint16_t this_len = MIN(chunk, (uint16_t)(len - off));

        if (bt_nus_send(conn, (const uint8_t *)&data[off], this_len) != 0) {
            /* Not subscribed, or no TX buffer free — stop. */
            break;
        }
    }
}

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

        if (sscanf(&buf[1], "%d,%d,%d", &r, &g, &b) != 3) {
            LOG_WRN("Bad color format: %s", buf);
        } else if (!in_byte_range(r) || !in_byte_range(g) || !in_byte_range(b)) {
            LOG_WRN("Color out of range: %d,%d,%d (valid 0-255)", r, g, b);
        } else {
            led_effects_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
        }
        break;
    }
    case 'b': {
        int bri = atoi(&buf[1]);

        if (!in_byte_range(bri)) {
            LOG_WRN("Bad brightness %d (valid 0-255)", bri);
        } else {
            led_effects_set_brightness((uint8_t)bri);
        }
        break;
    }
    case 'n': {
        /* Range-check before narrowing: a value above UINT16_MAX would
         * otherwise wrap into the valid range. */
        int count = atoi(&buf[1]);

        if (count < 1 || count > led_effects_max_pixels()) {
            LOG_WRN("Bad pixel count %d (valid 1-%d)",
                    count, led_effects_max_pixels());
        } else {
            led_effects_set_count((uint16_t)count);
        }
        break;
    }
    case '?': {
        /* Battery level is also published via the standard BLE Battery
         * Service; this is the human-readable version for a UART terminal. */
        char out[80];
        int  mv = battery_millivolts();
        int  n;

        if (mv < 0) {
            n = snprintf(out, sizeof(out), "batt: sampling\n");
        } else {
            n = snprintf(out, sizeof(out),
                         "batt %d mV (%u%%)%s | pixels %d | bri %u\n",
                         mv, battery_percent(),
                         led_effects_is_locked_out() ? " LOCKOUT" : "",
                         led_effects_pixel_count(),
                         led_effects_get_brightness());
        }

        if (n > 0) {
            nus_send_all(conn, out, (uint16_t)n);
        }
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
