# BL-LED companion app (Android)

A companion app that drives the BL-LED controller over BLE — every setting the
firmware exposes, from a phone. **Not started yet**; this directory is the
scaffold for the next person.

## What to build

A single-connection Android app that:

1. Scans for and connects to the controller (advertised name **`BL-LED`**).
2. Reads current state on connect and populates the UI.
3. Exposes controls for the everyday settings — **effect, colour, brightness,
   speed, auto colour-cycle** — plus an advanced/setup screen for **pixel count
   and ring calibration**.
4. Shows **battery** level.

## The contract

**Read [`../PROTOCOL.md`](../PROTOCOL.md) first — it is the authoritative wire
spec** and this app should be written against it, not against the firmware
source. It covers:

- the NUS service/characteristic UUIDs and the Battery Service,
- the **MTU chunking** rule (replies span multiple notifications — reassemble by
  newline),
- the exact command grammar (with ranges) and the `?`-response grammar,
- the recommended connect flow, and
- known limitations (notably: **commands are not acked** — validate input
  client-side against the documented ranges; there is no per-command reply
  except `?`).

## Suggested starting points

- **Language/UI:** Kotlin + Jetpack Compose.
- **BLE:** either raw `BluetoothGatt`, or a wrapper like Nordic's
  [Android-BLE-Library](https://github.com/NordicSemiconductor/Android-BLE-Library)
  / Kotlin-BLE-Library, which handle bonding, MTU, and notification plumbing.
- Model the protocol as a small typed layer (one function per command, one
  parser for the `?` report) so the UI never builds raw strings.
- **Permissions:** Android 12+ needs `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`
  runtime permissions; older needs location for scanning.

## Layout

Keep the Android project self-contained in this directory (`app/`), so the
firmware build at the repo root is unaffected. A root-level Gradle wrapper is
fine as long as it stays under `app/`.
