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

> **Never drive the data line into an unpowered strip.** Power the ring up
> before (or with) the board — never after. WS2812 data pins have ESD
> protection diodes to their own VDD rail, so if the board drives DIN high
> while the strip's VDD sits at 0 V, current flows backwards through that
> diode and tries to power the whole ring through one GPIO pin. The board
> browns out: solid LED, no USB, nothing on serial.
>
> This scales with pixel count, which makes it a convincing impostor for a
> firmware bug — 18 pixels of parasitic load the board survives, 36 kills it.
> It cost a long hunt through I2S buffer maths that never had a mechanism
> behind it. If the board dies the moment the ring is attached, check the
> ring's VDD before touching any code.

> **Do not power the ring from the `RAW` pin.** It reads ~4.24 V on a
> multimeter but collapses under load, leaving the ring completely dark. A
> meter draws no current, so it measures fine while being entirely broken.
> This is the same failure as above by another route: a collapsed rail is
> electrically an unpowered strip. Use a separate supply and share ground.

Ground **must** be shared: P0.06 swings 0–3.3 V relative to the board's
ground, so the ring needs that same reference to read the data line.

Current is the main constraint as the ring grows — roughly 60 mA per pixel at
full white, so 36 pixels can pull ~2 A. An inadequate supply shows up as
dimming or wrong colours toward the far end of the chain, flicker, or the
board resetting.

### Ring size

**The pixel count is set at runtime and persists** — send `N<count>` over BLE
(e.g. `N36`) and it is saved to flash, so one firmware image drives an 18, 36
or 72 pixel chain. No rebuild needed to change rings.

`chain-length` in `boards/nice_nano_nrf52840.overlay` is the compile-time
**maximum** (currently 72), because it sizes the static I2S DMA buffer. The
runtime count can be anything from 1 up to that ceiling. Raise it only if a
product needs more pixels than 72.

Multiple rings wire in **series**: DOUT of the first into DIN of the second,
and set the count to the total. The nRF52840 has only one I2S instance, so
chaining is the only way to drive more than one ring — they cannot be run as
independent strips.

Data for pixels beyond the active count is still clocked out — the driver
always transmits a full-length frame — but falls off the end of a shorter
chain harmlessly. This is deliberate; see the comment on `strip_flush()` in
`src/led_effects.c` for why a short frame would corrupt the display.

RAM cost is negligible at any realistic size. The I2S buffer is
`(3N + 1 + 12) × 4` bytes, double-buffered:

| max pixels | I2S buffer | mem slab |
|---|---|---|
| 18 | 268 B | 536 B |
| 36 | 484 B | 968 B |
| 72 | 916 B | 1832 B |

Power, not memory, is the limit that matters.

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
4. **`storage_partition` used to overlap the bootloader.** It was defined at
   `0xF4000` — exactly where the Adafruit UF2 bootloader starts — and was
   harmless only because nothing wrote to it. NVS erases every page it owns,
   so the first settings save would have destroyed the bootloader and left
   the board recoverable only over SWD. It now sits at `0xF0000–0xF4000`,
   carved out of the code partition. **Do not move it back.**

The onboard LED on P0.15 is **red** here, despite the board files calling it
"Blue LED". A separate blue LED exists that firmware does not control — it is
a power/charge indicator.

Each of these is explained where it is applied — see the comments in
`prj.conf` and `boards/nice_nano_nrf52840.overlay`.

---

## Known issues

**Debugging Bluetooth: `CONFIG_BT=y` without calling `bt_enable()` is not a
valid test.** The build links with `-Wl,--gc-sections`, so if nothing
references `bt_enable()` the linker discards most of the stack *including its
`SYS_INIT` entries*. That build boots perfectly while containing none of the
code under test — a false negative that cost hours.

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
| `B<0-255>` | `B128` | Set brightness. **Persisted** |
| `N<count>` | `N36` | Set pixel count (1–72). **Persisted** |

Persisted values are written to NVS ~750 ms after the last change (debounced,
so dragging a slider causes one write, not fifty) and restored at boot. NVS
skips writes of unchanged data, so re-saving an untouched field costs no flash
wear.

Brightness defaults to **64/255** until something is saved — deliberately low,
because a first boot lights every pixel and a full-brightness ring draws more
than most supplies will give.

Effect and colour are **not** persisted: the effect is stored as a bare index,
and renumbering `led_effect_t` would silently restore the wrong one. The boot
effect is set in `main.c`.

Out-of-range arguments are rejected and logged rather than wrapped. `B0` is
valid and means off — so **a ring that boots dark may simply have brightness 0
saved**, not broken hardware. There is currently no command to clear settings;
reconnect and send a higher `B` value.

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
├── boards/
│   ├── nice_nano_nrf52840.overlay  # I2S + WS2812 node, console, entropy
│   └── nicekeyboards/nice_nano/    # custom board definition
├── scripts/setup.sh
└── src/
    ├── main.c                      # boot, heartbeat, crash reporter
    ├── led_effects.c/.h            # effects engine
    └── ble_service.c/.h            # NUS command handler
```
