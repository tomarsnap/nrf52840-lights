# BL-LED Handover Notes

## Summary

This repo was set up to run WS2812 effects on an nRF52840 ProMicro-style board (nice!nano compatible).

A lot of failures were not build failures, but board/runtime/debugging mismatches:
- Mixed test context with an existing Kyria keyboard device on the same host.
- Flash copy permissions issues when mounting UF2 as root and copying as user.
- Early firmware images written to wrong flash offset and/or with incompatible assumptions.
- WS2812 SPI timing constants were adjusted to known Zephyr nRF sample values.

Current repo now includes a USB serial proof-of-life image to verify firmware execution independently of BLE and strip behavior.

Serial proof-of-life has now been confirmed on hardware (2026-07-22):
- UF2 copied successfully to /run/media/tomas/NICENANO.
- Device re-enumerated as /dev/ttyACM0.
- Heartbeat output observed:
  - BL-LED serial heartbeat 0
  - BL-LED serial heartbeat 1
  - BL-LED serial heartbeat 2
  - ...

## Hardware and Bootloader Facts Confirmed

From INFO_UF2 and USB:
- Board identifies as nice!nano bootloader.
- UF2 bootloader VID:PID observed: 239a:00b3.
- Board shows Adafruit nRF UF2 mass storage in bootloader mode.

## Key Troubleshooting Findings

1. Build environment was fixed and works
- west build works from the ncs workspace with:
  -DZEPHYR_TOOLCHAIN_VARIANT=gnuarmemb
  -DGNUARMEMB_TOOLCHAIN_PATH=/usr

2. Custom board definition was required
- nice_nano board was not available by default in this SDK setup.
- Board files were added under boards/nicekeyboards/nice_nano.

3. Overlay naming conflict was fixed
- Removed old boards/nice_nano.overlay naming conflict.
- Active overlay is boards/nice_nano_nrf52840.overlay.

4. WS2812 SPI bit pattern corrected
- Changed to Zephyr nRF sample-like values:
  - spi-one-frame = 0x70
  - spi-zero-frame = 0x40

5. Flash copy issue
- Copy to UF2 mount failed when using cp without sudo after sudo mount.
- Must use sudo cp (or mount as user).

6. USB disconnect + FAT write errors after copy
- Observed frequently after UF2 copy.
- This can happen when bootloader reboots immediately while host still flushes metadata.
- Treat with caution; verify runtime behavior separately.

7. BLE validation was inconclusive
- BL-LED advertisement not reliably observed in tests due overlapping host device context and ongoing runtime uncertainty.

## Current Code State (Important)

Current firmware is now strip-only (BLE still disabled).

Serial heartbeat mode was used to prove execution end-to-end and remains documented above as a known-good diagnostic fallback.

### Current behavior intended
- Enumerate as USB CDC ACM serial device.
- Wait for terminal DTR.
- Print heartbeat line once per second.

### Files changed for this test mode

- CMakeLists.txt
  - App sources reduced to main.c only.

- src/main.c
  - USB CDC serial heartbeat test program.
  - Uses:
    - usb_enable
    - uart_line_ctrl_get(DTR)
    - printk heartbeat

- prj.conf
  - Bluetooth disabled for isolation.
  - USB CDC console options enabled.
  - Logging disabled to reduce noise.

- boards/nice_nano_nrf52840.overlay
  - WS2812 SPI3 node still present.
  - Added:
    - chosen zephyr,console = cdc_acm_uart0
    - cdc_acm_uart0 child under &usbd

- app.overlay
  - Added earlier for CDC route, but build currently relies on board overlay updates.

## Last Verified Build

Built successfully (USB serial test image):
- Output UF2: build/zephyr/zephyr.uf2
- Start address: 0x1000

## Last Verified Runtime Result

USB serial runtime confirmed after flashing the above UF2:
- ttyACM device present: /dev/ttyACM0
- Repeated heartbeat lines observed from firmware.

Conclusion: build -> flash -> run pipeline is working.

## Flash Procedure That Worked Best

When board is in bootloader mode and appears as /dev/sdd and /run/media/tomas/NICENANO:

1. sudo mount /dev/sdd /tmp/nicenano
2. sudo cp build/zephyr/zephyr.uf2 /tmp/nicenano/
3. sync
4. sudo umount /tmp/nicenano

Notes:
- If mount is root-owned, plain cp will fail with Permission denied.
- USB disconnect right after copy is expected.

## Recommended Next Step for New Person

Goal: Prove board runtime first, then return to WS2812, then BLE.

1. Validate USB serial heartbeat
- Put board in bootloader.
- Flash current UF2.
- After reboot, check for ttyACM device:
  ls /dev/ttyACM*
- Read output:
  screen /dev/ttyACM0 115200
  or
  cat /dev/ttyACM0
- Expect lines like:
  BL-LED serial heartbeat N

2. If heartbeat works
- Firmware execution is confirmed.
- Next, reintroduce strip output in main only (no BLE yet).

### Current active next step (as of 2026-07-22)

Serial proof is complete. Next experiment is strip-only firmware:

1. Keep BLE sources disabled from build.
2. Re-enable led_effects in build/main.
3. Flash UF2 and verify WS2812 activity only.
4. If strip works, then reintroduce BLE service.

Status update:
- Strip-only code has been re-enabled in build sources.
- Strip-only UF2 build succeeded.
- Strip-only UF2 has been flashed to NICENANO.

### Strip-only result and follow-up (2026-07-22, later)

Reported symptom: board "goes full blue LED" once the strip is connected, and
the strip does nothing.

Root problem with diagnosing this: the strip-only image had NO output at all.
prj.conf sets CONFIG_USB_DEVICE_INITIALIZE_AT_BOOT=n, and the strip-only
main.c dropped the usb_enable() call that the heartbeat image had. With
CONFIG_LOG=n as well, that image enumerated no USB device and printed nothing.

Also relevant: the onboard LED on this board is a BLUE LED on P0.15
(see boards/nicekeyboards/nice_nano/nice_nano_nrf52840.dts). No firmware here
drives it. A solid or fading blue P0.15 therefore means the Adafruit
bootloader is in control, i.e. the board reset into DFU — consistent with a
brownout when the strip is connected.

Changes made:
- src/main.c rewritten as a staged bring-up diagnostic (see file header).
- src/led_effects.c: effect thread no longer auto-starts at boot (it used to
  race main and drive the strip before device_is_ready was ever checked).
  led_effects_init() now returns 0/-ENODEV and starts the thread only on
  success. fill() now propagates the driver's return code.
- src/led_effects.h: added led_effects_pixel_count(), led_effects_direct_fill(),
  led_effects_direct_pixel() for diagnostics.
- prj.conf: CONFIG_GPIO=y for the P0.15 heartbeat.

Build verified: FLASH 43796 B, RAM 22336 B, UF2 start 0x1000.
NOT yet flashed — board was not attached when this was built.

## BLE WORKING (2026-07-22, 20:53) — use Zephyr's SW link layer, NOT the SDC

Confirmed on hardware: advertises as "BL-LED", controller reports HCI 5.4,
identity F1:C7:83:D0:6A:49, and the ring keeps running alongside it.

### Root cause: Nordic's SoftDevice Controller crashes this board AT BOOT

With `CONFIG_BT_LL_SOFTDEVICE` (the NCS default), the board died before
`main()` — no USB, solid LED, fault reason 19 (K_ERR_ARM_MEM_DATA_ACCESS).
The SDC pulls in MPSL, which initialises via SYS_INIT at boot; that is what
faulted. It is NOT `bt_enable()` and NOT anything in ble_service.c.

Fix, in prj.conf:

    CONFIG_BT_CTLR=y
    CONFIG_BT_LL_SW_SPLIT=y     # Zephyr's software controller, no MPSL

Three knock-on fixes were required, each a real incompatibility:

    CONFIG_HW_CC3XX=n           # CC310 lib won't link against picolibc with
                                # the gnuarmemb toolchain (undefined memmove,
                                # bad ARM/Thumb relocation)
    CONFIG_ENTROPY_CC3XX=n
    CONFIG_ENTROPY_NRF5_RNG=y   # CC3XX entropy driver needs nrfxlib headers
    CONFIG_BT_CTLR_LE_ENC=n     # no pairing/encryption needed

and in the board overlay, because the SoC dtsi points entropy at the now
absent CryptoCell:

    chosen { zephyr,entropy = &rng; };

### A METHODOLOGY TRAP THAT COST HOURS — read this

"`CONFIG_BT=y` but never call `bt_enable()`" is NOT a valid bisection. The
build links with `-Wl,--gc-sections`, so if nothing references `bt_enable()`
the linker discards most of the BT stack INCLUDING its SYS_INIT entries. That
build boots perfectly and proves nothing — it does not contain the code under
test. This false result led to a long chain of wrong conclusions about
`bt_enable()` being the crash site.

The observation that broke it open: USB never enumerated even with a 30 s
delay before `bt_enable()`, though enumeration takes 2-3 s. A per-second
countdown log confirmed the board died before printing anything at all.

Also disproven along the way: a very tidy correlation across seven builds
where everything under ~32 KB RAM worked and everything over it crashed. It
was coincidence — a 37 KB build with BLE unreferenced booted fine.

## Superseded: earlier BLE notes (2026-07-22, 20:26)

State at end of session: the LED side is finished and verified. Bluetooth is
not. Everything below is established by bisection on hardware.

### THE KEY FINDING: SPI and BLE fail with the SAME fault

`bt_enable()` crashes with reason 19 = **K_ERR_ARM_MEM_DATA_ACCESS** — read
off the LED crash blinker as 3 long blinks then 20 short.

That is the IDENTICAL fault code that any SPI transfer produces on this board.
These are almost certainly ONE root cause, not two coincidences: an MPU
data-access violation, i.e. the CPU touching memory the MPU has not mapped.

Anything that explains one should be checked against the other. If the real
cause is ever found and fixed, RE-TEST SPI — the I2S workaround and its 10 ms
drain wait could then be dropped for the conventional SPI driver.

Tried and did NOT fix it:
- `CONFIG_ARM_MPU=n` — board does not boot at all with the MPU off.
- `CONFIG_HW_STACK_PROTECTION=n` (theory: the Cortex-M4's 8 MPU regions being
  exhausted by per-thread stack guards, leaving memory unmapped). Result
  unconfirmed at end of session.
- `CONFIG_MAIN_STACK_SIZE=8192` (for the SPI case) — no effect.

### An unexplained inconsistency — resolve this first

USB never enumerates in ANY build that calls `bt_enable()`, even though
`usb_enable()` runs first and there is a deliberate 8 second delay before the
Bluetooth call. Enumeration normally completes in 2-3 s, so the crash appears
to happen EARLIER than `bt_enable()` — which contradicts the model that
`bt_enable()` is what crashes.

The bisection build (identical, minus the `ble_service_init()` call)
enumerates fine and logs normally. One of these assumptions is wrong. Find out
which before theorising further.

### What is known

- `CONFIG_BT=y` with the whole stack COMPILED IN boots perfectly. Verified:
  ring runs, USB enumerates, console logs, heartbeat blinks. So the BT stack
  merely being present is NOT the problem.
- Calling `bt_enable()` hangs the board hard. USB never enumerates, the
  heartbeat LED sits SOLID (so the scheduler never gets to run threads), and
  nothing is ever printed.
- It hangs the same way with `bt_enable(NULL)` (synchronous) and with
  `bt_enable(cb)` (asynchronous). The callback form was tried specifically to
  stop a stuck controller taking USB and the LEDs down with it — it does not
  help, so the block is INSIDE bt_enable before it returns.
- Suspicion is SoftDevice Controller / MPSL bring-up, most likely around the
  low-frequency clock, but this is NOT confirmed.

### Related change, effect unproven

`boards/nicekeyboards/nice_nano/nice_nano_nrf52840_defconfig` now sets:

    CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y
    CONFIG_CLOCK_CONTROL_NRF_K32SRC_500PPM=y

The board files previously specified an external 32.768 kHz crystal
(`K32SRC_XTAL`) that this board does not have. That is a genuine bug worth
keeping fixed regardless. It did NOT fix the `bt_enable()` hang, so do not
assume it is related.

### Where to start next time

1. Reproduce: call `bt_enable()`, confirm solid LED + no USB.
2. The bisection harness is the fastest tool — comment out the
   `ble_service_init()` call in main.c to get a known-good booting build back.
3. Suspects not yet investigated: MPSL clock configuration, whether the
   Adafruit bootloader's MBR interferes with the controller, and whether
   `CONFIG_BT_LL_SOFTDEVICE` needs different RAM/flash placement given
   `CONFIG_USE_DT_CODE_PARTITION=y` puts the app at 0x1000.

## WORKING (2026-07-22, 20:15) — full effect sequence runs on hardware

Confirmed visually: red ring, green, blue, single white pixel walking the
chain, full white ring, then continuous rainbow. The hardware is an 18-pixel
WS2812 RING, not a strip.

### The configuration that works — do not change casually

1. **I2S, not SPI.** `worldsemi,ws2812-i2s` on `i2s0`, SDOUT on P0.06,
   SCK/LRCK parked on P0.22/P0.24 (unused, they just toggle).
   SPI is UNUSABLE on this board: any transfer, down to a single byte, on
   either SPIM1 or SPIM3, crashes the CPU with K_ERR_ARM_MEM_DATA_ACCESS —
   with the ring disconnected. That fault was never explained, only avoided.

2. **`extra-wait-time = <10000>`** (10 ms) on the ws2812 node. The 300 us
   default is far too short: the peripheral is still draining when the next
   write arrives and it is rejected with -EIO / "Cannot write in state: 3"
   (I2S_STATE_STOPPING).

3. **NEVER add `i2s_trigger(..., I2S_TRIGGER_DROP)` between frames.** It was
   tried as a way to force the state machine back to READY. It makes things
   strictly worse: DROP calls `nrfx_i2s_stop()` but leaves teardown to the
   completion handler, so the following START fails with nrfx INVALID_STATE
   (0x0bad000c) and nothing is sent after the first frame. See the comment on
   `strip_flush()` in src/led_effects.c.

4. **Power the ring from an EXTERNAL supply, never from RAW.** RAW measures
   4.24 V open-circuit but collapses under load — a multimeter draws no
   current, so it reads fine while the ring gets nothing. This is why the ring
   stayed dark through hours of firmware debugging. Share GND between the
   supply and the board; do NOT connect external 5 V to RAW or VCC.
   WS2812B wants 3.5-5.3 V; a 3 V battery lights it but is under spec.

### Wrong turns worth not repeating

- The onboard LED this firmware drives (P0.15) is RED on this board, though
  the board files call it "Blue LED". The BLUE LED is not firmware-controlled
  — it is a power/charge indicator and reacts to load on RAW. Hours were lost
  watching the wrong LED.
- The RESET button was converting every crash into a reboot, which made a
  crash look like a reset loop and sent the investigation after watchdogs and
  brownouts. Removing it exposed the real failure.
- USB CDC serial is an unreliable witness: it drops output silently and goes
  quiet on re-enumeration, so silence means nothing. The LED blink-count in
  main.c was added for this reason and is the instrument to trust.
- Bit-banging WS2812 from GPIO was written and then abandoned. It works in
  principle but disables interrupts for ~540 us per frame, which would wreck
  BLE timing later. Do not resurrect it for this project.

## CONFIRMED: reboot loop, not a hang (2026-07-22, 19:00)

The board was never hanging. It REBOOTS, every 20-35 s, always immediately
after the first `led_strip_update_rgb()`. Confirmed by watching USB
enumeration: the device number increments on every cycle.

Decisive timing evidence that it tracks the code and is NOT a fixed-period
watchdog: inserting ~10 s of extra work before the SPI write lengthened the
reset period from 22 s to 34 s — exactly the amount inserted.

Also confirmed: driving P0.06 slowly as plain GPIO (six 200 ms pulses) does
NOT reset the board. Only the 4 MHz SPI burst does.

### RESETREAS reading is still OUTSTANDING — earlier one was contaminated

A reading of `RESETREAS = 0x1 (RESET-PIN)` was captured at 19:07 and briefly
mistaken for the root cause. It was almost certainly the USER physically
double-tapping the reset button at that moment (two resets 1 s apart, ending
in the bootloader). It says nothing about the SPI-write reset.

TO GET A CLEAN READING: let the board free-run through at least one full
reset cycle WITHOUT touching the reset button, then read the RESETREAS line
printed on the following boot. src/main.c prints it three times per boot
because CDC ACM drops bytes.

Still-live hypotheses for the SPI-write reset:
- brownout / supply dip when the SPIM starts switching (RESETREAS would be 0)
- CPU lockup, i.e. a double fault (RESETREAS LOCKUP bit)
- something asserting the reset pin for a real electrical reason
Note the reset button is an SMD part on the breadboard, so a long floating
"antenna" wire on RST is NOT a plausible mechanism here.

### How to read RESETREAS again if needed

`report_reset_reason()` in src/main.c reads and clears it via
`nrf_power_resetreas_get/clear()` from `<hal/nrf_power.h>`. It must be read
early and cleared, because the register is sticky across resets.

### Findings from the diagnostic runs (2026-07-22, evening)

Confirmed on hardware, in order:

1. Firmware runs. Enumerates as `239a:0101 BL-LED WS2812 diagnostic`, prints
   its banner. Boot, USB stack and console all fine.
2. The WS2812 driver initialises cleanly: `LED strip ready: 18 pixels`.
   SPI bus binds, DTS node resolves, colour mapping validates.
3. Execution stops at the FIRST `led_strip_update_rgb()` call, every time.
   Last line is always `-> update_rgb all-off ...` with no result line.
4. Not a power problem. Identical with the strip connected and disconnected;
   the board never re-enumerates, so it is not resetting or browning out.
5. NOT SPIM3-specific. Moving the strip from spi3 to spi1 changed nothing —
   same hang at the same call. Instance errata are ruled out.
6. USB CDC output dies permanently at that same moment. The port stays
   enumerated but yields zero bytes; a fresh open 10 s later gets nothing.

### IMPORTANT: USB CDC serial is not a trustworthy witness here

This wasted significant time. Zephyr's CDC ACM `poll_out` silently DROPS
bytes when the device is suspended or the ring buffer cannot flush. Absent
output was repeatedly misread as "the CPU is dead" when the kernel was in
fact still running (onboard LED still blinking).

Also: a crash dump can never arrive over CDC ACM. The fatal-error handler
halts with interrupts disabled, and USB needs interrupts to transmit — so
enabling CONFIG_LOG does NOT buy visible fault dumps on this transport.

Consequence: `src/main.c` encodes progress as a BLINK COUNT on the onboard
blue LED (P0.15) — blink N times, pause, repeat. That readout survives USB
failure and is the instrument to trust. Counts are the STAGE_* defines at
the top of main.c.

Open question at handover: whether the first strip write blocks with the
kernel alive, or hard-faults. The blink count answers it — a stuck count of
2 means blocked-but-alive; a frozen LED means fault.

Note the driver SHOULD NOT block forever: CONFIG_SPI_COMPLETION_TIMEOUT_
TOLERANCE=200 makes spi_context_wait_for_completion() bound the wait to
~201 ms for this transfer and return -ETIMEDOUT. That it apparently does
not is itself a clue worth chasing.

What the diagnostic distinguishes:
- Blue P0.15 blinking crisply at 1 Hz  -> this firmware is running.
- Blue P0.15 solid/fading, no blink    -> bootloader has control; board reset.
- Serial reports strip not ready       -> DTS/driver problem, not wiring.
- Serial clean but strip dark          -> wiring / 3.3V level shift / ground.
- Dies only at a later (brighter) step -> power, not data.

3. If heartbeat does not work
- Issue is still below app level (boot/runtime/USB or board mismatch).
- Re-check board pinout variant and bootloader/runtime transitions.

4. Once strip-only works
- Reintroduce BLE service files and BT config gradually.

## Known Caveats

- This board is ProMicro-style and may not map exactly to official nice!nano assumptions.
- If board is physically attached to keyboard matrix or shared wiring, some pins may not be free.
- Keep test board physically isolated while validating strip and serial.
