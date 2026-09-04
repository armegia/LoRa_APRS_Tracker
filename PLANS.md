# Working notes / future plans (T-Beam Supreme fork)

Personal backlog while getting familiar with this codebase on a LilyGO T-Beam Supreme v3
(ESP32-S3, SX1262, AXP2101). Not upstream-facing, just tracking intent between sessions.

Priority order (chosen 2026-09-04): **2 → 3 → 4 → 1**. Frequencies are already set correctly
on both test devices (Europe region), so the web-GUI "cosmetics" items (4, then 1) come last.
Items 1-4 below (per that priority) are all done and flashed as of 2026-09-04. Milder cases
found during the item-4 audit (battery/GPS inert toggles, the LoRa `power`-field ambiguity) are
being deliberately left alone, not treated as bugs to fix.

## Known issues — documented, explicitly NOT being fixed yet

**TNC2 mode over BLE doesn't work in practice (2026-09-04).** The firmware does implement it —
[ble_utils.cpp](src/ble_utils.cpp) fully branches on `Config.bluetooth.useKISS` for both RX
(`MyCallbacks::onWrite`) and TX (`txToPhoneOverBLE`), and TNC2 mode advertises the standard
Nordic UART Service UUID ([ble_utils.cpp:37](src/ble_utils.cpp#L37), commented "ANDROID - BLE
Terminal app"). In practice it didn't work with the Android app being tested. Not yet known
whether that's an app-side limitation (doesn't actually support this BLE profile) or a genuine
firmware bug in the TNC2-over-BLE path — needs more research before touching any code.
Decision: stick to BLE + KISS for now; do not attempt a fix until root-caused further.

## 5. BLE bonding/pairing — attempted, REVERTED (2026-09-04)

**Problem:** every time devices were switched, Android required re-pairing, and with no
security at all *any* nearby BLE device could write into the RX characteristic and get this
tracker to transmit arbitrary content over RF under the station's licensed callsign. Confirmed
via nRF Connect (connected with zero pairing prompt) that `ble_utils.cpp` never requested
security at all — no `setSecurityAuth()` call, no `_ENC`/`_AUTHEN` characteristic properties.

**What was tried, in order:**

1. `NimBLEDevice::setSecurityAuth(true, false, true)` (bonding, no MITM/"Just Works" since this
   hardware has no display/keypad, secure connections) + `READ_ENC`/`WRITE_ENC` on the
   characteristics, relying on the library's built-in behavior of auto-starting security when a
   client subscribes to an `_ENC`-flagged characteristic
   ([NimBLEServer.cpp:409-427](.pio/libdeps/ttgo_t_beam_s3_SUPREME_v3/NimBLE-Arduino/src/NimBLEServer.cpp#L409-L427)).
   Result: no pairing ever happened (`onAuthenticationComplete` never fired), yet notify data
   still flowed — the passive trigger didn't reliably fire in practice.
2. Proactively forcing it via `NimBLEDevice::startSecurity()` in a new
   `onConnect(NimBLEServer*, ble_gap_conn_desc*)` override, called the instant a client connects.
   Result: worse — the connection became unstable (repeated
   connect/disconnect/reconnect cycling visible in the serial log, confirmed fresh after
   restarting the Android device) and the phone showed "cannot connect" errors. Root cause,
   from reading `ble_gap_security_initiate()` in the NimBLE source
   ([ble_gap.c:5930-5935](.pio/libdeps/ttgo_t_beam_s3_SUPREME_v3/NimBLE-Arduino/src/nimble/nimble/host/src/ble_gap.c#L5930-L5935)):
   a BLE **peripheral cannot force pairing**, only send an SM "Security Request" hint via
   `ble_sm_slave_initiate()` — Android's handling of unsolicited peripheral-initiated Security
   Requests is known to be inconsistent, and sending it immediately on connect (before service
   discovery settles) appears to actively destabilize the link on this phone rather than just
   being ignored.

**Decision: reverted entirely.** `ble_utils.cpp` is back to its original unauthenticated state
(confirmed via `git diff` showing zero changes). This is a known-hard problem in the
ESP32/NimBLE + Android BLE bonding space more broadly — Meshtastic (a comparable ESP32 LoRa
project) has reportedly hit similar issues — so it needs real research before trying again, not
another blind attempt. The spec-correct trigger (a rejected read/write on an `_ENC`
characteristic forces the *central* to pair, which Android is actually obligated to honor,
unlike an unsolicited peripheral request) was never actually tested end-to-end, since the
proactive-connect approach introduced instability before that path could be isolated. Worth
revisiting: test attempt 1's approach (passive `_ENC` flags, no proactive `startSecurity()`)
specifically by having the phone app *write* to the tracker (not just receive), since only a
write should trigger the spec-guaranteed pairing path.

**Authorization gap from this is still open and unaddressed:** the RX characteristic remains
unauthenticated — anything in BLE range can still write to it. Not fixed, not currently being
worked on.

## 1. Dynamically selectable logging (no reflash required) — DONE (syslog deferred)

**Problem (was):** log level was fixed at compile time via a commented-out `#define DEBUG` in
`LoRa_APRS_Tracker.cpp`. Getting DEBUG-level output meant editing source + full rebuild +
reflash every time.

**Implemented:**

- [x] Added a persisted `logLevel` int field to `Config` (`include/configuration.h`,
      `src/configuration.cpp` read/write/defaults — default `6` = INFO).
- [x] `setup()` now calls `logger.setDebugLevel(static_cast<logging::LoggerLevel::Value>(Config.logLevel))`
      unconditionally ([LoRa_APRS_Tracker.cpp](src/LoRa_APRS_Tracker.cpp)) — the old
      `#define DEBUG` compile-time switch is gone.
- [x] Added a "Serial Log Level" dropdown (Error/Warn/Info/Debug) to the web config UI
      (`data_embed/index.html`, `data_embed/script.js`, POST handler in `src/web_utils.cpp`).
      Takes effect on next reboot, same as every other setting in this UI (there's no live-apply
      mechanism anywhere in this codebase, so this matches existing behavior).
- [x] Verified: builds clean under `-Werror -Wall`, flashed to the Supreme, confirmed via splash
      screen date bump that the flash actually took.

**Deferred (explicitly, per 2026-09-04 decision — not doing yet):**

- [ ] `peterus/esp-logger` already supports a UDP syslog sink (`setSyslogServer`) — wiring that
      up for remote log capture without a USB cable. Revisit later if useful.

**To use:** WiFi AP config portal → set "Serial Log Level" to Debug → Save → device reboots →
open serial monitor → reboot again (or power-cycle) to catch the DEBUG-level boot-time lines,
since most of them only fire once during `setup()`.

## 2. Surface RSSI/SNR for received packets — DONE

**Status (was):** TX/RX packet *content* was already logged at INFO level
(`lora_utils.cpp:190` and `:258`), but `receivePacket()` computed `.rssi`, `.snr`,
`.freqError` on every received packet without ever logging them.

**Implemented:**

- [x] Added a DEBUG-level log line right after those fields are captured
      ([lora_utils.cpp:263-264](src/lora_utils.cpp#L263-L264)):
      `RSSI: %d dBm / SNR: %.2f dB / FreqError: %d Hz`.
- [x] Verified on real hardware: a packet from `EA4HXZ-2` decoded with
      `RSSI: -38 dBm / SNR: 5.50 dB / FreqError: 90 Hz` — strong, clean signal.

**Root cause found (2026-09-04):** not a bug in this codebase. The peer beacon (an NRF-based
tracker, different/closed firmware, source not available) had stale/wrong saved LoRa
configuration; a full factory reset on that device fixed it. Confirms this device's radio
stack, frequency plan, and RX path were correct all along — the RSSI/SNR log line is what made
that clear instead of guessing.

## 3. Show region name next to each LoRa frequency slot in the web GUI — DONE

**Problem (was):** the web UI numbered each LoRa config slot generically ("1)", "2)", "3)",
"4)" — [script.js:207](data_embed/script.js#L207) — with no indication of which region each
one is. The device's own OLED display *does* label them, via a switch keyed on the same array
index in [lora_utils.cpp:86-91](src/lora_utils.cpp#L86-L91): index 0 = `EU/WORLD`,
1 = `POLAND`, 2 = `UK`, 3 = `US`.

**Implemented:**

- [x] Added a `LORA_REGION_LABELS` array at the top of `script.js` mirroring the C++ switch,
      with a comment pointing back at `lora_utils.cpp` so the two don't silently drift apart.
- [x] Each slot's numbering now renders as `1) EU/WORLD`, `2) POLAND`, etc.; falls back to a
      plain `N)` for any slot beyond the default 4.
- [x] Widened the label column (`col-1` → `col-3 col-md-2`) so the region name doesn't
      character-wrap in the narrow original column.
- [x] Builds clean, flashed to the Supreme.

**Still open (not done, deferred):** the two label lists (C++ switch and JS array) are still
two separate copies of the same data, just now both documented and pointing at each other.
Real unification (e.g. build-time injection from C++ into the embedded JS) would fold into
item 4's capability/data-exposure work below if that's ever tackled.

## 4. Web GUI shows settings the compiled firmware doesn't support — mechanism DONE (Bluetooth wired; audit of other capabilities still open)

**Problem (was):** `data_embed/index.html` + `script.js` are a single static blob baked into
every board's firmware by `tools/compress.py` — there's no per-board templating and no
endpoint that reports actual compiled-in capabilities. Confirmed concretely with Bluetooth:
`bluetooth.useBLE` was shown unconditionally on every board, and on boards without
`HAS_BT_CLASSIC` (all ESP32-S3 boards — no Classic BT radio exists on that chip) setting it to
`false` drove execution into an `#ifdef` block that compiles to nothing. Bluetooth silently did
nothing — no error, no fallback.

**Capability audit (2026-09-04), for the record:** Bluetooth is the only *broken* case found —
a config value pointing at code that doesn't exist. Milder relatives, not fixed here: battery
voltage monitoring (`battery.sendVoltage`/`monitorVoltage`) is inert (not broken) on boards
without `BATTERY_PIN`/`HAS_AXP192`/`HAS_AXP2101`; GPS settings are likewise inert on
`HAS_NO_GPS` TNC-only boards. Different issue entirely, worth a separate look someday: the
per-slot LoRa `power` (dBm) field means something different depending on `HAS_1W_LORA` — same
number, different real-world output, [lora_utils.cpp:79](src/lora_utils.cpp#L79) vs
[:156](src/lora_utils.cpp#L156). Touchscreen/joystick ruled out — no corresponding `Config`
toggle exists for those, so nothing to misconfigure.

**Implemented — the generic mechanism, designed to be upstream-PR-able since it needs zero
per-board web changes and self-updates correctly for boards nobody here can test:**

- [x] New `GET /capabilities` endpoint ([web_utils.cpp](src/web_utils.cpp)) returning a small
      JSON object built from the *same* `#ifdef HAS_BT_CLASSIC` guard already used everywhere
      else in this codebase — so it can't drift from what's actually compiled in (the compiler
      itself decides the answer, not a separate parser).
- [x] Generic client-side gating in `script.js`: any element tagged
      `data-requires-capability="X"` is hidden unless `capabilities.X` is true; a sibling
      tagged `data-requires-capability-fallback="X"` shows in its place. Adding a new gated
      capability later is one line in the C++ endpoint + one attribute in HTML — no per-board
      forking of the web UI.
- [x] Wired up for Bluetooth: the BLE/Classic toggle is hidden and replaced with a "BLE only"
      note on boards without `HAS_BT_CLASSIC`.
- [x] Defensive backend guard also added in the POST handler
      ([web_utils.cpp](src/web_utils.cpp), mirrors the existing
      [configuration.cpp:329-334](src/configuration.cpp#L329-L334) default-value guard) — even
      a raw POST (bypassing the UI entirely) can no longer force `useBLE=false` on a board
      without Classic BT.
- [x] Builds clean under `-Werror -Wall`, flashed to the Supreme.

**Still open:**

- [ ] Visual confirmation on real hardware that the toggle is actually hidden and the fallback
      note shows (pending — can't reach the device's own WiFi AP from this session).
- [ ] Audit the milder cases above (battery/GPS) and decide whether they're worth wiring up
      the same way, or leave as-is since they're inert rather than broken.
- [ ] This was built and tested only on the Supreme (`hasBTClassic: false` path). The
      `hasBTClassic: true` path (an original-ESP32 board) is untested — exactly the kind of
      thing worth a PR so someone with that hardware can confirm it.

## Debugging setup reference

- Board enumerates as **COM70** (native ESP32-S3 USB CDC, `ARDUINO_USB_CDC_ON_BOOT=1`).
- `pio device monitor -e ttgo_t_beam_s3_SUPREME_v3 --port COM70` or VS Code's PlatformIO
  serial monitor icon; both pick up `monitor_speed=115200` +
  `monitor_filters=esp32_exception_decoder` from `platformio.ini`.
- `PYTHONIOENCODING=utf-8` and `~/.platformio/penv/Scripts` on PATH are set in both
  `~/.bashrc` and the PowerShell profile — new terminals should have `pio` working directly.
- Flashing over the native USB port worked with no manual BOOT-button bootloader entry needed —
  `USB mode: USB-Serial/JTAG` auto-resets fine on this board.
- Splash screen `versionDate` ([LoRa_APRS_Tracker.cpp:72](src/LoRa_APRS_Tracker.cpp#L72)) is a
  handy "did the flash actually take" visual check — shows briefly at boot, no delay added
  (kept fast on purpose, per 2026-09-04 decision).
