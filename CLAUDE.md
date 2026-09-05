# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32 Arduino firmware (PlatformIO) turning LoRa+GPS hardware into an APRS tracker/station.
Single shared C++ application in `src/`, built for ~40 different board variants via
per-variant config in `variants/<name>/` (see Architecture below). Config reference for
end users: the project wiki linked from `README.md` (installation, tracker settings, menu
guide) — useful background when a change touches user-facing behavior, but not something to
fetch by default.

## Build / flash

PlatformIO Core lives in the VS Code PlatformIO IDE extension's private venv, not on PATH by
default: `~/.platformio/penv/Scripts/pio.exe` (an env with `pio`/`platformio` and
`PYTHONIOENCODING=utf-8` on PATH may already be set up in the shell profile — if `pio` isn't
found, fall back to the full path).

```bash
export PYTHONIOENCODING=utf-8   # required on Windows/Git Bash - pio otherwise throws UnicodeEncodeError
pio run -e <env>                              # build only
pio run -e <env> -t upload --upload-port COMx # build + flash
pio device monitor -e <env> --port COMx       # serial monitor (115200, exception decoder configured)
pio device list                               # find the port
```

The root `platformio.ini`'s `default_envs` points at one specific board (currently
`ttgo-t-beam-v1_2`) — **always pass `-e <env>`** explicitly for whichever board you're
actually targeting; env names match `variants/<name>/` directory names.

**Before flashing, check the target COM port isn't already held open** by a serial monitor
(VS Code's PlatformIO panel or a CLI `pio device monitor`) — upload fails with a
busy-port/`PermissionError` otherwise. Quick check (PowerShell):

```powershell
try { $p = New-Object System.IO.Ports.SerialPort COMx; $p.Open(); $p.Close(); "FREE" } catch { "BUSY" }
```

`common_settings.ini` sets `-Werror -Wall` globally — any new compiler warning fails the
build, not just warns.

No test suite / linter beyond the compiler itself.

## Architecture

**Multi-board via `#define HAS_*` capability macros, not runtime detection.** Each
`variants/<name>/board_pinout.h` declares its hardware via plain preprocessor defines —
`HAS_SX1262`/`SX1268`/`SX1276`/`SX1278`/`LLCC68` (radio chip), `HAS_BT_CLASSIC` (only
original ESP32 boards — ESP32-S3 has no Classic Bluetooth radio at all, so this is never
definable there), `HAS_AXP192`/`HAS_AXP2101` (PMU), `HAS_TFT`/`HAS_TOUCHSCREEN`/
`HAS_JOYSTICK`, `HAS_GPS_CTRL`/`HAS_NO_GPS`, `HAS_1W_LORA`, `HAS_TCXO`, `BUTTON_PIN`. `src/*.cpp`
branches on these with `#ifdef` throughout — grep for the macro name across `src/` before
assuming a feature is universally available. `variants/<name>/platformio.ini` extends the
root `[env:esp32]` (which itself extends `common_settings.ini`'s `[common]`) and is pulled in
via the root `platformio.ini`'s `extra_configs = variants/*/platformio.ini`.

**The web config UI is one static blob shared by every board — it doesn't know what's
actually compiled in.** `data_embed/{index.html,script.js,style.css,...}` are the sources;
`tools/compress.py` (a PlatformIO pre-build script) gzips them and bakes the result into
every firmware image identically, substituting a `%BUILD_INFO%` placeholder in `index.html`
along the way. Because it's one blob for all boards, a control can be shown for a feature
that isn't compiled in on the connected board — this has caused real bugs (e.g. a Bluetooth
BLE/Classic toggle silently doing nothing on boards without `HAS_BT_CLASSIC`). The fix
pattern in use: `GET /capabilities` (`src/web_utils.cpp`) reports what's actually compiled in
via the same `#ifdef` guards used elsewhere, and `script.js` hides/disables any element
tagged `data-requires-capability="<name>"` accordingly — extend this pattern rather than
adding more unconditional controls. Also mirror any such guard into the POST handler in
`web_utils.cpp` (defense in depth — the client-side hide isn't the only thing preventing an
unsupported value from being saved).

**Config is a single `Configuration` object (`include/configuration.h` /
`src/configuration.cpp`), persisted as `/tracker_conf.json` on SPIFFS**, editable through the
same web UI (served over a self-hosted WiFi AP, `WIFI_Utils::checkIfWiFiAP()` in
`wifi_utils.cpp`, triggered by a menu option or an unset callsign). Adding a field means
touching four places in lockstep: the struct in `configuration.h`, read/write/defaults in
`configuration.cpp` (`readFile()`/`writeFile()`/`setDefaultValues()` — note the `needsRewrite`
null-check pattern that forces a rewrite+reboot when a field is missing from an old saved
file), the POST handler in `web_utils.cpp`, and the form + JS binding in `data_embed/`.

**Radio abstraction is RadioLib** (`src/lora_utils.cpp`), one physical driver class
(`SX1262`/`SX1268`/etc.) selected at compile time by the `HAS_*` macro above. Logging uses the
project-owned `include/logger.h` + `src/logger.cpp` implementation (global `logger`), which
formats complete records and serializes the final write across FreeRTOS tasks. Its level is set
at boot from persisted `Config.logLevel` (runtime-changeable via the web UI, no reflash needed)
rather than a compile-time flag. Keep application diagnostics on this logger instead of calling
`Serial.print*()` directly; the logger also enables compiler checking of printf arguments.

## Git remotes — hard rule, not a preference

`origin` is the public upstream (`github.com/richonguzman/LoRa_APRS_Tracker`) — **never push
there**. `gitea` is the user's private checkpoint remote. `github` is the user's public GitHub
fork (`github.com/armegia/LoRa_APRS_Tracker`) and is the destination for explicitly approved
future-PR branches. Never open an upstream pull request or push a branch without confirming the
requested destination and scope first.

## Session notes

`PLANS.md` at the repo root tracks in-progress investigation notes, decisions (including
things tried and deliberately reverted), and a prioritized backlog from ongoing work on a
LilyGO T-Beam Supreme v3 (`ttgo_t_beam_s3_SUPREME_v3`, COM70 in that environment, native
ESP32-S3 USB-CDC — no BOOT-button bootloader entry needed, auto-reset works). It's a living,
session-scoped doc, not a substitute for reading current code — check it for context on *why*
something looks mid-flight, but verify against actual code state before relying on it.

The Android BLE/KISS client used for this work is specifically the NA7Q APRSdroid `stable`
branch (<https://github.com/na7q/aprsdroid/tree/stable>), whose
`src/backend/BluetoothLETnc.scala` implements BLE. Do not confuse it with official/upstream
APRSdroid, which does not support BLE.
