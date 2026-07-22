# bl-led — nRF52840 WS2812 BLE LED Controller

Drives a WS2812 LED ring from a **nice!nano**-style nRF52840 board over BLE,
using the Nordic UART Service (NUS).

## Features

- Effects: solid colour, rainbow, breathe, off
- BLE control via Nordic UART Service
- Simple ASCII command protocol
- WS2812 driven over **I2S** (see the gotchas — SPI does not work on this board)

---

## Hardware

### Wiring

| WS2812 ring | Connect to |
|---|---|
| DATA | **P0.06** on the board |
| 5V | **External 5 V supply** — see below |
| GND | Supply ground **and** board GND (must be common) |

> **Do not power the ring from the `RAW` pin.** It reads ~4.24 V on a
> multimeter but collapses under load, leaving the ring completely dark. A
> meter draws no current, so it measures fine while being entirely broken.
> This cost hours of debugging. Use a separate 5 V supply and share ground.

Ground **must** be shared: P0.06 swings 0–3.3 V relative to the board's
ground, so the ring needs that same reference to read the data line.

Current is the main constraint as the ring grows — roughly 60 mA per pixel at
full white, so 36 pixels can pull ~2 A. An inadequate supply shows up as
dimming or wrong colours toward the far end of the chain, flicker, or the
board resetting.

### Ring size

Set `chain-length` in `boards/nice_nano_nrf52840.overlay`. Everything else —
effects, buffers, rainbow spread — derives from it automatically.

---

## Board-specific gotchas

This board is a ProMicro-style clone and does **not** match stock nice!nano
assumptions. These were found the hard way and are load-bearing:

1. **SPI cannot drive the ring.** Any SPI transfer, down to a single byte, on
   either SPIM instance, crashes the CPU with `K_ERR_ARM_MEM_DATA_ACCESS` —
   even with nothing connected. Unexplained. I2S is used instead.
2. **Bluetooth needs Zephyr's software link layer.** Nordic's SoftDevice
   Controller brings up MPSL at boot and crashes this board before `main()`
   runs. `CONFIG_BT_LL_SW_SPLIT=y` avoids it.
3. **There is no 32.768 kHz crystal**, though the board files originally
   claimed one. `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` is required for BLE.

The onboard LED on P0.15 is **red** here, despite the board files calling it
"Blue LED". A separate blue LED exists that firmware does not control — it is
a power/charge indicator.

Full detail, including the wrong turns, is in `HANDOVER_NOTES.md`.

---

## Building

Requires the nRF Connect SDK in `~/ncs` (see `scripts/setup.sh`).

```bash
source ~/ncs/.venv/bin/activate
export ZEPHYR_BASE=~/ncs/zephyr
cd ~/ncs
west build -d ~/projects/bl-led/build -b nice_nano/nrf52840 ~/projects/bl-led \
    -- -DZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb -DGNUARMEMB_TOOLCHAIN_PATH=/usr
```

Output: `build/zephyr/zephyr.uf2`. The toolchain flags are only strictly
needed on a pristine build, but passing them always is harmless.

---

## Flashing

1. Double-tap **RESET**. The board mounts as a USB drive named `NICENANO`.
2. Copy the firmware:

```bash
cp build/zephyr/zephyr.uf2 /run/media/$USER/NICENANO/ && sync
```

The board reboots into the new firmware automatically.

---

## BLE usage

Advertises as **`BL-LED`**. Connect with any BLE UART app — **Serial Bluetooth
Terminal** or **nRF Toolbox → UART** are far easier than nRF Connect for this.

In nRF Connect: expand **Nordic UART Service**, tap the ↑ icon on the
characteristic ending `...0002` (the writable one), and **switch the format to
`Text (UTF-8)`** before sending — it defaults to hex.

### Commands

Case-insensitive ASCII.

| Command | Example | Description |
|---|---|---|
| `E<0-3>` | `E2` | Effect: 0=off, 1=solid, 2=rainbow, 3=breathe |
| `C<r>,<g>,<b>` | `C255,0,128` | Set colour (0–255 per channel) |
| `B<0-255>` | `B128` | Set brightness |

---

## Diagnostics

USB CDC serial on this board is unreliable — it drops output silently and goes
quiet on re-enumeration, so **silence proves nothing**. The onboard LED is the
signal to trust:

| LED | Meaning |
|---|---|
| 1 Hz blink | Running normally |
| 3 slow blinks, pause, N short | Crashed; N = fault reason + 1 (20 = reason 19, `K_ERR_ARM_MEM_DATA_ACCESS`) |
| Solid or dark | Died before the fault handler could run |

Note that WS2812s hold their last frame while powered, so a lit ring does not
prove the firmware is running — it may be displaying a stale frame.

---

## Project structure

```
bl-led/
├── CMakeLists.txt
├── prj.conf                        # Kconfig — heavily commented, load-bearing
├── HANDOVER_NOTES.md               # debugging history and hardware findings
├── boards/
│   ├── nice_nano_nrf52840.overlay  # I2S + WS2812 node, console, entropy
│   └── nicekeyboards/nice_nano/    # custom board definition
├── scripts/setup.sh
└── src/
    ├── main.c                      # boot, heartbeat, crash reporter
    ├── led_effects.c/.h            # effects engine
    └── ble_service.c/.h            # NUS command handler
```
