# bl-led — nRF52840 WS2812 BLE LED Controller

Drives a WS2812 LED ring from a **nice!nano**-style nRF52840 board over BLE,
using the Nordic UART Service (NUS).

## Features

- Effects: solid, rainbow, breathe, worm, sparkle, aurora, heartbeat,
  firefly, pinwheel, confetti, off
- BLE control via Nordic UART Service
- Simple ASCII command protocol
- WS2812 driven over **I2S** (see the gotchas — SPI does not work on this board)

---

## Hardware

### Wiring

| WS2812 ring | Connect to |
|---|---|
| DATA | **P0.29** on the board |
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

Ground **must** be shared: P0.29 swings 0–3.3 V relative to the board's
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

### Two rings and mirroring

The active pixel count is split evenly into **`LED_RINGS` (currently 2)** logical
rings — ring 0 is the first half of the chain, ring 1 the second. This is a
*view* over the single framebuffer, **not** a second buffer: mirrored and
per-ring effects cost no extra pixel RAM. If the count is odd or not divisible
(e.g. `N35`), the split degrades to one ring spanning the whole strip so effects
still render.

Effects address rings by **logical position** — "so many pixels clockwise from
this ring's top" — and the engine maps that to a physical LED using the ring's
calibration. An effect draws with `led_all_set()` (same position on every ring)
or `led_ring_set()` (one ring), and never touches a physical index. See the
drawing API in `src/led_animations.h` and `render_worm()` in
`src/led_animations.c` as the worked example (two worms, one per ring, running
opposite directions).

**Calibration.** Because the two rings can be mounted at any rotation and wound
either way, each ring has a **top** (which physical pixel is its 12-o'clock) and
a **direction** (±1). Set them once per build with `K<ring>,<top>,<dir>`; they
persist. Mirror symmetry is just the two rings calibrated with **opposite**
directions — then `led_all_set()` reflects; same direction copies.

To find a ring's top pixel, use `I<pixel>` to light one physical LED at a time
and walk it around until it sits at the mounting's top, then read the index off
the `?` report and feed it to `K`. `I-1` (or selecting any effect) leaves
identify mode.

### Battery monitoring

**Off by default.** Fit the divider, then set `status = "okay"` on the `vbatt`
node in `boards/nice_nano_nrf52840.overlay`:

```
BAT+ ──[ 100k ]──┬──[ 100k ]── GND
                 │
               P0.02 (AIN0)
```

A cell reaches 4.2 V, above the SAADC's 3.6 V ceiling, so it must be divided.
100k/100k draws 21 µA (~15 mAh/month, nothing against 3500 mAh). Both values
are read from devicetree, so any ratio works — edit `output-ohms` (lower leg)
and `full-ohms` (both legs).

Once enabled, level is published via the standard **BLE Battery Service** (any
app reads it natively) and by the `?` command. Percentage comes from a Li-ion
discharge curve, not a linear map — the curve is flat from ~90% to ~20%, so a
linear gauge reads ~50% for most of the runtime then collapses. Readings under
load sag, so it reads low while the LEDs are bright; pessimistic is the safe
direction.

Below 3.1 V for three consecutive samples the LEDs are blanked. This latches
until reboot on purpose: blanking removes most of the load, the cell rebounds
above the threshold within seconds, and an unlatched check would oscillate.

> **Why it is opt-in rather than auto-detected.** An unwired SAADC pin does not
> read zero — measured on this board it wandered between 2.1 V and 2.8 V, which
> are entirely plausible voltages for a dying cell. No threshold can separate
> "not fitted" from "flat", so the firmware believed ~2.6 V and blanked a
> perfectly healthy strip. Presence of hardware has to be declared, not
> inferred. While `vbatt` is disabled the code compiles to stubs.

> **This is a gauge, not protection.** It cannot disconnect anything — nothing
> in the battery's current path is under firmware control — and it only runs
> while the firmware does. Blanking the LEDs does **not** stop the drain: a
> WS2812 draws ~1 mA even showing black, so 72 pixels still pull ~70 mA
> afterwards. Only a hardware switch removes the load, and only a protection
> PCB guards against over-discharge and short circuit when the MCU is dead.

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
5. **A USB CDC console resets the board on flash writes when headless.** With
   USB CDC as the console, writing flash (any settings save — `N`/`B`) while no
   host is draining the port hard-resets the SoC on *every* save. An attached
   terminal masks it completely (the host keeps the USB endpoint drained), so it
   reads as an intermittent bug that only appears in the field — exactly the
   production configuration. The app therefore brings up neither USB nor a
   console; the LED and BLE are the only interfaces. Re-enable USB for dev only,
   with a terminal attached.

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

> Building a client (e.g. the planned companion app)? See **[`PROTOCOL.md`](PROTOCOL.md)**
> — the authoritative wire spec: NUS UUIDs, MTU chunking, the exact command and
> `?`-response grammar, and known limitations. The table below is the
> human-facing summary; `PROTOCOL.md` is what an app should be written against.

In nRF Connect: expand **Nordic UART Service**, tap the ↑ icon on the
characteristic ending `...0002` (the writable one), and **switch the format to
`Text (UTF-8)`** before sending — it defaults to hex.

### Commands

Case-insensitive ASCII.

| Command | Example | Description |
|---|---|---|
| `E<0-10>` | `E2` | Effect: 0=off, 1=solid, 2=rainbow, 3=breathe, 4=worm, 5=sparkle, 6=aurora, 7=heartbeat, 8=firefly, 9=pinwheel, 10=confetti |
| `C<r>,<g>,<b>` | `C255,0,128` | Set colour (0–255 per channel) |
| `B<0-255>` | `B128` | Set brightness. **Persisted** |
| `S<0-255>` | `S200` | Set animation speed (0=slowest, 255=fastest). **Persisted** |
| `A<0\|1>` | `A1` | Auto colour cycle: re-randomise the colour each animation cycle. **Persisted** |
| `N<count>` | `N36` | Set pixel count (1–72). **Persisted** |
| `K<ring>,<top>,<dir>` | `K0,5,1` | Calibrate a ring: physical top pixel + winding (+1/−1). **Persisted** |
| `I<pixel>` | `I5` | Identify: light one physical pixel (negative = off) |
| `?` | `?` | Report battery, geometry and effect state |

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

**The production build is headless — no USB, no serial console** (see gotcha 5
below). The onboard LED is the only local signal, and BLE `?` reports live
state. For log output, build a dev image with USB re-enabled (re-enable the
console block in `prj.conf` and `usb_enable()` in `main.c`) — but only with a
terminal attached, or flash writes will reset the board.

The onboard LED is the signal to trust:

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
├── PROTOCOL.md                     # BLE wire spec — write app clients against this
├── prj.conf                        # Kconfig — heavily commented, load-bearing
├── boards/
│   ├── nice_nano_nrf52840.overlay  # I2S + WS2812 node, console, entropy
│   └── nicekeyboards/nice_nano/    # custom board definition
├── app/                            # companion Android app — scaffold, not started
├── scripts/setup.sh
└── src/
    ├── main.c                      # boot, heartbeat, crash reporter
    ├── led_animations.c            # the effects themselves — edit this to add one
    ├── led_animations.h            # render contract + ring drawing API (led_frame)
    ├── led_effects.c/.h            # engine: state, persistence, threading, strip write
    ├── battery.c/.h                # SAADC gauge + critical-voltage lockout
    └── ble_service.c/.h            # NUS command handler
```

### Adding an effect

Effects are isolated from the engine behind a one-frame render contract, so
adding one touches only two places and needs no knowledge of NVS, threading or
I2S:

1. Add a value to `led_effect_t` in `src/led_effects.h` (and bump `EFFECT_MAX`).
2. Write a `render_*()` in `src/led_animations.c` that paints the frame and
   returns the delay (ms) until the next frame, then add a `case` for it to
   `led_render()`.

The engine snapshots the live state under its lock and hands each renderer an
immutable `struct led_frame` — `pixels`, `count`, the selected `colour`
(`r`/`g`/`b`, set by `C`), `brightness` (`B`), `speed` (`S`) and the ring
geometry — then flushes the buffer and sleeps for the returned interval. All of
those are global to every effect; an effect uses whichever it needs and ignores
the rest (the rainbow, for instance, synthesises its own hues and ignores the
colour).

**Drawing.** Pick the call by intent, and let the engine handle physical
indices:

- `led_all_set(f, pos, col)` — same logical position on **every** ring. With
  opposite per-ring direction calibration this mirrors; this is the primitive
  for symmetric effects.
- `led_ring_set(f, ring, pos, col)` — one ring, for effects that differ between
  the two (see `render_worm`). Out-of-range rings are ignored, so a two-ring
  effect is harmless on a one-ring build.
- `led_px_set(f, i, col)` — a raw physical pixel, for whole-strip effects that
  ignore ring boundaries.
- `led_clear(f)` — blank the strip first if the effect lights only a few pixels.
- `led_cycle_color(f)` — call at a natural **cycle boundary** (the instant the
  animation returns to its start). When the user has enabled auto colour cycling
  (`A1`), this swaps in a fresh random colour for the next cycle; otherwise it
  does nothing, so it is always safe to call. Put it where the strip is dark for
  a seamless swap (see the wrap in `render_breathe`). Effects that ignore the
  global colour (rainbow) need not call it.

`led_ring_len(f)` / `led_ring_count(f)` report the geometry so an effect is
written once and works at any ring size.

`speed` is WLED-style: 0–255, no fixed meaning of its own. The usual way to
consume it is `led_speed_delay(speed, fast_ms, slow_ms)`, which maps it to the
frame delay you return, but an effect is free to use it however it likes (e.g.
to scale how far it advances per frame). A renderer runs on a single thread, so
per-effect animation phase can be a plain file-scope `static` (see
`render_rainbow`). Pixels past the active count are blanked by the engine, so an
effect only ever fills `pixels[0 .. count-1]`.
