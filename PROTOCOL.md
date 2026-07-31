# BL-LED BLE Protocol

The wire contract for talking to the controller — everything a companion app
(Android or otherwise) needs, without reading the firmware. The firmware side
lives in `src/ble_service.c`; **if you change the protocol there, change it
here too.**

The next planned task is an **Android app that drives every setting over these
commands.** This document is written for that developer.

---

## Transport

All control happens over the **Nordic UART Service (NUS)** — a two-characteristic
"serial cable over BLE". The app **writes ASCII command strings** to one
characteristic and **subscribes for ASCII replies** on the other. Battery level
is also published separately via the standard Battery Service.

### Scanning

Advertised name: **`BL-LED`** (`CONFIG_BT_DEVICE_NAME`). It advertises
connectable, general-discoverable, BR/EDR not supported. There is no custom
service UUID in the advertisement — filter by name.

### GATT services

| Service | UUID | Use |
|---|---|---|
| Nordic UART (NUS) | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | commands + replies |
| Battery Service (BAS) | `0x180F` | battery %, native |

**NUS characteristics:**

| Characteristic | UUID | Properties | Direction |
|---|---|---|---|
| RX | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Write / Write Without Response | **app → device** (send commands here) |
| TX | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | Notify | **device → app** (replies arrive here) |

To receive replies you **must enable notifications** on TX (write `0x0001` to its
CCC descriptor) — Android does this via `setCharacteristicNotification()` +
writing the CCCD. Commands with no reply (everything except `?`) work without it,
but enable it anyway so `?` works.

**Battery:** read/subscribe the Battery Level characteristic (`0x2A19`) under BAS
for a 0–100 % value pushed on change. This is the easiest way to show battery;
`?` also reports voltage if you want it.

### MTU and chunking — important

This peripheral never raises the ATT MTU, so it stays at the default **23 bytes,
i.e. a 20-byte notification payload.** Replies longer than that (the `?` report
always is) are **split across several notifications** by the firmware. The app
**must reassemble**: concatenate TX notification payloads until you have a
complete line (ends in `\n`). Do not assume one notification == one reply.

If your BLE stack negotiates a larger MTU, the firmware honours it (it chunks to
`MTU − 3`), so reassembly by newline works at any MTU.

---

## Commands

ASCII, **case-insensitive** (`E2` == `e2`), sent to the **RX** characteristic. A
trailing `\n`/`\r` is stripped and optional. One command per write; max 63 bytes.

Numeric arguments are **range-checked before use** — an out-of-range or malformed
command is ignored (and logged over serial, which is disabled in production, so
**the app gets no error back** — see *Limitations*). Validate client-side.

| Command | Arg range | Example | Effect | Persisted |
|---|---|---|---|---|
| `E<n>` | `0`–`11` (see enum) | `E2` | Select effect | no |
| `C<r>,<g>,<b>` | each `0`–`255` | `C255,0,128` | Set colour | no |
| `B<n>` | `0`–`255` | `B128` | Brightness (`0` = off) | **yes** |
| `S<n>` | `0`–`255` | `S200` | Animation speed (`0` slow … `255` fast) | **yes** |
| `A<n>` | `0`/`1` (non-zero = on) | `A1` | Auto colour cycle | **yes** |
| `N<n>` | `1`–`72` | `N36` | Active pixel count | **yes** |
| `K<ring>,<top>,<dir>` | see *Geometry* | `K0,5,1` | Calibrate a ring | **yes** |
| `I<pixel>` | `-1`–`71` | `I5` | Identify one physical pixel (`-1` = off) | no |
| `?` | — | `?` | Report full state (multi-line reply) | — |

**Effect enum** (`E` argument):

| n | Effect |
|---|---|
| 0 | off |
| 1 | solid |
| 2 | rainbow |
| 3 | breathe |
| 4 | worm |
| 5 | sparkle (travelling gradient + sparkle, ring 1 randomised) |
| 6 | aurora (slow flowing gradient, tinted by the colour) |
| 7 | meteor (twin meteors crossing, one per ring) |
| 8 | heartbeat (lub-dub pulse in the colour) |
| 9 | firefly (sparse twinkles fading in and out) |
| 10 | pinwheel (counter-rotating colour wedges) |
| 11 | confetti (random self-coloured pops) |

> The set of effects **grows over time** — new ones are appended with higher
> indices. Don't hard-code "4 is the max": read `effect` from `?` and treat any
> value you don't recognise as "some effect". Ideally fetch the range from a
> future capability, but for now hard-coding names 0–4 with a graceful fallback
> is fine.

**Persistence:** persisted values are debounced ~750 ms after the last change,
then written to flash and restored on boot. Effect and colour are **not**
persisted (they reset to the boot default), which is why the app should read them
back with `?` on connect rather than assuming.

### Geometry (`K` / `I`)

The strip is one physical WS2812 chain mounted as **2 equal rings** (pixels
`[0, size)` and `[size, 2·size)`). Each ring has a calibrated **top** (the
physical pixel index at its 12-o'clock) and **direction** (`+1`/`-1`). `K` sets
them; `top` must be a physical index inside that ring, `dir` must be `±1`, else
the command is rejected. `I<pixel>` lights one physical pixel so you can find a
ring's top by eye; selecting any effect (or `I-1`) leaves identify mode.

Most apps only need to expose these on an "advanced/setup" screen — the everyday
controls are `E`/`C`/`B`/`S`/`A`.

---

## The `?` report

Reply to `?`. Two `\n`-terminated lines, arriving across multiple notifications
(reassemble by newline). Grammar is a stable **`key value` list**, fields
separated by ` | `. Parse by splitting on `|`, then on whitespace; do **not**
rely on fixed offsets or field order beyond what's documented — but the field
set below is stable.

**Line 1 — state:**

```
batt 3987 mV (86%) | pixels 36 | effect 3 | color 255,0,0 | bri 64 | spd 128 | auto 1
```

| Field | Meaning |
|---|---|
| `batt <mv> mV (<pct>%)` | Battery millivolts and percent. May instead be `batt: sampling` before the first ADC reading, **or** carry a trailing ` LOCKOUT` token when the battery-critical cutoff has blanked the strip. If battery hardware isn't fitted this still prints, but the value is meaningless — prefer BAS. |
| `pixels <n>` | Active pixel count (the `N` value) |
| `effect <n>` | Current effect index (the `E` value) |
| `color <r>,<g>,<b>` | Current colour (the `C` value) |
| `bri <0-255>` | Brightness |
| `spd <0-255>` | Speed |
| `auto <0/1>` | Auto colour cycle on/off |

**Line 2 — ring geometry:**

```
rings 2 x 36 | r0 top 5 dir +1 | r1 top 40 dir -1
```

`rings <count> x <size>`, then one ` | r<i> top <t> dir <±d>` block per ring.
(If the pixel count isn't divisible into 2 rings the firmware degrades to a
single ring here.)

**Suggested regexes** (tolerant of the optional `LOCKOUT`/`sampling` variants):

```
batt (\d+) mV \((\d+)%\)( LOCKOUT)?   # or:  batt: sampling
pixels (\d+)
effect (\d+)
color (\d+),(\d+),(\d+)
bri (\d+) | spd (\d+) | auto ([01])
rings (\d+) x (\d+)
r(\d+) top (\d+) dir ([+-]\d+)
```

---

## Recommended app connect flow

1. Scan for name `BL-LED`, connect.
2. (Optional) request a larger MTU — replies then arrive in fewer notifications.
3. Enable notifications on **TX** and on the **Battery Level** characteristic.
4. Send `?`, reassemble the two lines, populate the whole UI from them.
5. On each control change, send the one-letter command. Optimistically update the
   UI; optionally re-send `?` to confirm (there's no per-command ack).

---

## Limitations / notes for the next dev

- **No command acknowledgement.** Only `?` replies. A malformed or out-of-range
  command is silently ignored (it's logged only over the serial console, which is
  **off in production**). The app must range-check inputs itself and use `?` to
  verify. Adding an ack/nack notification is a reasonable future firmware change.
- **No unsolicited state push** (except battery via BAS). If two clients could be
  connected, or state changes on its own (e.g. auto-colour cycling changes the
  live colour every animation cycle — it is **not** re-reported), poll `?` to
  refresh. In practice one phone connects at a time.
- **The live colour drifts under `A1`.** With auto colour cycle on, the colour
  reported by `?` keeps changing on its own; a colour picker should reflect that
  it's not authoritative while `auto 1`.
- **Effect/colour are not persisted**; everything else is. Don't assume the
  device came up with the colour you last set.
```
