# Working notes / future plans (T-Beam Supreme fork)

Personal backlog while getting familiar with this codebase on a LilyGO T-Beam Supreme v3
(ESP32-S3, SX1262, AXP2101). Not upstream-facing, just tracking intent between sessions.

## Android BLE test client — important

All APRSdroid BLE/KISS investigation and hardware results in this file refer specifically to
the **NA7Q APRSdroid `stable` branch**:
<https://github.com/na7q/aprsdroid/tree/stable>. This is not the official/upstream APRSdroid,
which does not support BLE. The tested NA7Q implementation is
`src/backend/BluetoothLETnc.scala` and uses the standard BLE KISS UUIDs implemented here.

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

## 5. BLE bonding/pairing — DONE, LEGACY-PIN COMPATIBILITY MODE (2026-09-05)

**Problem:** every time devices were switched, Android required re-pairing, and with no
security at all *any* nearby BLE device could write into the RX characteristic and get this
tracker to transmit arbitrary content over RF under the station's licensed callsign. Confirmed
via nRF Connect (connected with zero pairing prompt) that `ble_utils.cpp` never requested
security at all — no `setSecurityAuth()` call, no `_ENC`/`_AUTHEN` characteristic properties.

**What was tried, in order:**

1. `NimBLEDevice::setSecurityAuth(true, false, true)` (bonding, no MITM/"Just Works" because
   the firmware has no BLE passkey/confirmation UI, secure connections) + `READ_ENC`/`WRITE_ENC` on the
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

**2026-09-04 decision:** reverted entirely at the time. This is a known-hard problem in the
ESP32/NimBLE + Android BLE bonding space more broadly — Meshtastic (a comparable ESP32 LoRa
project) has reportedly hit similar issues — so it needs real research before trying again, not
another blind attempt. The spec-correct trigger (a rejected read/write on an `_ENC`
characteristic forces the *central* to pair, which Android is actually obligated to honor,
unlike an unsolicited peripheral request) was never actually tested end-to-end, since the
proactive-connect approach introduced instability before that path could be isolated. Worth
revisiting: test attempt 1's approach (passive `_ENC` flags, no proactive `startSecurity()`)
specifically by having the phone app *write* to the tracker (not just receive), since only a
write should trigger the spec-guaranteed pairing path.

**Hardware investigation 2026-09-05:** APRSdroid stable was checked against its actual
`BluetoothLETnc.scala` source. Its KISS service and characteristic UUID directions match this
firmware. It subscribes to the encrypted notify characteristic, expects the first authorization
failure, and reconnects once. Its UI can report a locally queued position before a GATT write
has actually reached the tracker, so tracker-side `BLE Tx` is the delivery evidence.

The instrumented tests established the following:

1. Android Settings could put the T-Beam in "saved devices" without running BLE SMP. The
   tracker saw an unencrypted connection and disconnect, no encryption-change event, and zero
   stored bonds. The generic Android dialog also offered calls/contacts access, which is not
   meaningful for this BLE KISS service.
2. APRSdroid then touched the encrypted characteristic. The first security attempt failed with
   NimBLE status 7 (`ENOTCONN`); its reconnect waited about 22 seconds and failed with status 13
   (`ETIMEOUT`). No KISS write reached the tracker.
3. Starting security immediately from `onConnect` allowed APRSdroid to establish temporary
   encryption and the tracker received/transmitted its KISS frames. There were no additional
   prompts during that live session. A tracker reset proved this was **not bonding**: boot still
   logged `stored bonds: 0`, and the next reconnect failed again.
4. Secure Connections + MITM + a fixed display-only passkey (`123456`) made Android request the
   PIN, but NimBLE-Arduino 1.4.1 stalled and timed out. Repeating while entering the PIN in under
   10 seconds ruled out human delay; encryption never completed and no bond was stored.

**Final implementation, hardware verified:** use
authenticated Legacy Passkey pairing with PIN `123456`, plus explicit ENC+ID key distribution
in both directions (`setSecurityInitKey(3)` and `setSecurityRespKey(3)`). The characteristics
require both encryption and authentication. This is compatible with the maintained NimBLE
secure-server example and works around the failing Secure Connections exchange, but is a real
security tradeoff: a captured initial legacy pairing is weaker than LE Secure Connections and
the static six-digit PIN can be brute-forced. The user explicitly approved testing this build.
If this compatibility mode ever becomes unacceptable, the next isolated step is migrating
NimBLE-Arduino 1.4.1 to the current 2.x API and returning to Secure Connections; do not mix that
larger migration into unrelated changes.

The callbacks now log the exact GAP security status text, peer address, encryption/
authentication/bond state, key size, and stored bond count. Startup logs all bond addresses at
DEBUG level. The old 100 ms delays were removed from NimBLE host callbacks.

Recovery is available from Web Configuration → Device → **Clear BLE bonds**. This writes a
one-shot SPIFFS marker and reboots; after NimBLE initializes on the next normal boot it calls
`deleteAllBonds()`, removes the marker, and logs how many bonds were cleared.

**Acceptance test passed on hardware:** Android requested PIN `123456` once. The initial link
reported `encrypted=1 authenticated=1 bonded=1 keySize=16`; after a physical tracker reset,
startup reported `stored bonds: 1` and restored identity `94:45:60:54:a7:4b`. APRSdroid then
reconnected with `encrypted=1 authenticated=1 bonded=1 keySize=16 storedBonds=1`, without a PIN,
re-pair, or authorization prompt. Two manually sent position frames produced paired `BLE Tx`
and `LoRa Tx` records and appeared on the T-Beam display. Persistent bonding is confirmed.

### Random pairing PIN follow-up — DONE, HARDWARE VERIFIED (2026-09-05)

The fixed `123456` PIN is replaced by a hardware-random six-digit PIN for each physical BLE
connection. Existing bonded peers should restore their saved keys without showing or using the
new PIN; an unbonded peer receives the PIN prepared immediately before `startSecurity()`.

The NimBLE host callbacks do not access the display. They publish the requested PIN through
atomic integer state, and `BLE_Utils::handlePairingDisplay()` performs all formatting and I2C
display work from the Arduino main loop. While active, it shows `BLE PAIR` and the PIN,
refreshes once per second, suppresses the normal menu redraw, and wakes/holds on a display that
eco mode had switched off. Authentication completion, disconnect, or a 60-second UI timeout
clears the pairing screen. The timeout does not alter the BLE procedure itself; after it clears,
the normal configured display timeout resumes.

The hardware acceptance sequence was: forget both bonds, confirm the screen PIN matches Android
pairing, verify authenticated bonding and KISS/LoRa traffic, reboot without forgetting, verify
silent bonded reconnection and traffic, then repeat a clean pairing with a second random PIN.

**First hardware attempt:** failed safely with `storedBonds=0`. NimBLE used the configured
random passkey internally, so entering the old `123456` produced status 1028 (confirm-value
mismatch), but no PIN appeared. NimBLE-Arduino 1.4.1's `NimBLEServer.cpp` only invokes
`onPassKeyRequest()` for `DISPLAY_ONLY` when its configured passkey equals the library's
`123456` compatibility sentinel, and its separate GAP listener did not receive the consumed
passkey action. The implementation now leaves that sentinel configured and returns/publishes
the per-connection random PIN from `onPassKeyRequest()`. Here, `123456` is only an internal
library switch, not the pairing PIN, except for the one-in-900,000 chance that the RNG itself
selects that number.

**Second hardware attempt:** random PIN `118282` appeared in the serial log and on the OLED,
and Android completed with `encrypted=1 authenticated=1 bonded=1 keySize=16`. The immediate
callback still reported `storedBonds=0`, which may precede NimBLE's asynchronous NVS write and
must be checked after reset. The size-2 `BLE PAIRING` OLED header wrapped because it was 132
pixels wide on a 128-pixel display and partially obscured the PIN. It is shortened to the
non-wrapping `BLE PAIR`, with the final line explicitly formatted as `PIN: NNNNNN`.

Further testing of that bond passed before reset. The peer disconnected and reconnected under
resolved identity `94:45:60:54:a7:4b`; encryption restored in about 575 ms with no passkey
request, and the completion callback then reported `storedBonds=1`. A position and an APRS
message traveled from APRSdroid over BLE and were transmitted over LoRa. The second tracker's
ACK was received over LoRa and delivered back to APRSdroid over BLE. This verifies persistent
storage, silent bonded reconnection within the same boot, and bidirectional KISS traffic. The
remaining persistence test is reconnection after the screen-fix flash/reset.

**Post-flash/reset persistence passed:** after flashing the screen correction (which resets the
ESP32 but preserves NVS), the same identity `94:45:60:54:a7:4b` reconnected without a passkey
request. Encryption restored in about 573 ms with `encrypted=1 authenticated=1 bonded=1
keySize=16 storedBonds=1`, and another APRSdroid position traveled through `BLE Tx` and
`LoRa Tx`. Bond persistence across a firmware upload/reset is verified. Only a fresh pairing
remained to visually verify the non-wrapping layout and a second random PIN.

**Final clean pairing passed:** the log-to-file monitor captured random PIN `748043`, followed
by successful encryption and `encrypted=1 authenticated=1 bonded=1 keySize=16`, with no warning
or error records. This differs from the first random PIN (`118282`), and the clean pairing was
completed using the corrected non-wrapping `BLE PAIR` screen. The random-PIN feature and its
hardware acceptance sequence are complete.

**PR-readiness audit:** added GPL headers to the project-owned logger, validation for persisted
and submitted log levels, INFO/DEBUG separation so PIN digits are not logged at the default INFO
level, and an explicit warning that the bond count sampled inside the authentication callback
can precede NimBLE's NVS commit. BLE source comments are now self-contained rather than linking
to this private notebook. `PULL_REQUEST_DRAFT.md` records the complete maintainer-facing scope,
tradeoffs, sanitized test evidence, provenance, and attribution.

Representative `-Wall -Werror` builds now pass for the Supreme, original ESP32 T-Beam,
ESP32-C3 DIY tracker, Heltec ESP32-S3 TNC, and T-Deck Plus environments. Only the Supreme has
been exercised end-to-end on hardware.

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

**Concurrency/formatting repair (2026-09-05, hardware tested):** removed the
`peterus/esp-logger` dependency and added a project-owned API-compatible logger. The old logger
wrote one logical record using many separate `Serial.print()` calls, allowing the Arduino loop,
`nimble_host`, and `async_tcp` tasks to tear each other's lines. It also reused one `va_list`
across two `vsnprintf()` calls, which is undefined behavior. The replacement formats safely
with `va_copy()`, prefixes each record with milliseconds/task/core, and writes the entire record
under one mutex in one `Stream::write()` call. Active direct `Serial.print*()` diagnostics were
migrated to the same logger; only commented debugging examples remain. Enabling GCC printf
format checking also exposed and fixed four existing calls that passed Arduino `String` objects
to `%s` or used runtime text as the format string. The logger remains disabled during global
construction and is explicitly enabled after `Serial.begin()` so configuration loading cannot
touch the USB stream before the Arduino core initializes it. Hardware traces from the Arduino
loop and `nimble_host` tasks confirmed that complete timestamp/task/core-tagged records are
emitted. ANSI color codes were subsequently removed so captured logs remain plain text across
PlatformIO, PowerShell, and non-terminal sinks.

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
