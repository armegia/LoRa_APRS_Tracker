# Draft pull request: diagnostics, board-aware configuration, and secure BLE TNC pairing

> Internal preparation document. Copy/adapt this text when opening the upstream pull request;
> do not include this file in Ricardo's branch unless he asks for it.

## Proposed title

Improve runtime diagnostics, board-aware configuration, and BLE TNC bonding

## Summary

This started as an annoyance, not a security audit: the Android APRSdroid (NA7Q fork) client
kept demanding BLE re-authorization on every connection. Investigating why turned up the actual
cause - BLE characteristics accepted connections with no authentication or encryption at all -
which is also a real security gap: any nearby BLE device could write a KISS frame and get the
tracker to transmit under the configured amateur-radio callsign. This PR fixes both the
annoyance and the gap behind it, and along the way also fixes debug logging (previously a
compile-time switch requiring a reflash) and a shared web UI that could show/save settings a
given board doesn't actually support.

**What changed, in one line each:**

- Runtime-configurable, thread-safe logging, replacing a removed third-party dependency.
- `GET /capabilities` + generic web-UI gating, so the config page only shows/accepts settings the
  connected board actually supports.
- Authenticated, persistently-bonded BLE pairing with a hardware-random on-screen PIN, plus a
  "Clear BLE bonds" recovery action.
- A pre-submission code review (`PR_REVIEW_NOTES.md`) that found and fixed 15 further
  correctness/consistency issues before this draft was finalized.

**Tested:** builds clean (project's `-Wall -Werror`) on 5 board environments; hardware-verified
end-to-end — pairing, bond persistence across reset, bidirectional KISS traffic — on the T-Beam
Supreme v3 only. See "Verification performed" below for both.

**Main tradeoff:** uses BLE Legacy Pairing, not LE Secure Connections, because NimBLE-Arduino
1.4.1 plus the tested Android client stalled on Secure Connections. See "Authenticated persistent
BLE bonding" below.

**Status:** draft, not yet opened upstream — see "Before opening the pull request" at the bottom.

Everything past this point is detail and decision history for anyone who wants it: motivation,
changes, what was tried and rejected, verification evidence, known limitations, and attribution.

---

## Upstream and scope

- Upstream repository: `richonguzman/LoRa_APRS_Tracker`
- Upstream base audited: `bfd531a` (`origin/main`, 2026-09-05 fetch)
- Primary hardware: LilyGO T-Beam Supreme v3 / ESP32-S3 / SX1262 / AXP2101
- PlatformIO environment: `ttgo_t_beam_s3_SUPREME_v3`
- Android interoperability client: NA7Q APRSdroid `stable`, specifically its BLE KISS
  backend (`src/backend/BluetoothLETnc.scala`)
- Official/upstream APRSdroid is not the client used here; it does not provide this BLE path.

This work intentionally does not modify APRSdroid. It keeps the existing BLE KISS UUIDs and
addresses reliability, observability, configuration, and pairing in the tracker firmware.

## Motivation

The investigation started because the Android APRSdroid (NA7Q fork) client kept demanding BLE
re-authorization on every connection - an annoyance, not a security concern, at first glance.
Chasing why it happened is what turned up problem 4 below. Several other user-visible problems
were already being worked on the day before and are included here too:

1. Debug logging required editing a compile-time switch and reflashing. Log records from the
   Arduino loop, NimBLE host, and AsyncTCP tasks could interleave on the serial stream.
2. Received LoRa signal metrics were captured but not logged, making radio diagnosis harder.
3. The shared web UI displayed bare frequency-slot numbers and controls unsupported by some
   compiled boards. On ESP32-S3, selecting Classic Bluetooth could save an impossible setting
   and silently disable Bluetooth behavior.
4. Every BLE reconnect required re-authorizing the phone from scratch, because BLE
   characteristics accepted connections without any authentication or encryption at all, so no
   durable bond was ever established.
5. A fixed pairing passkey would improve compatibility but would be weak and awkward. The
   tracker has a display and hardware RNG, so it can provide a fresh visible passkey.

The security issue has an RF consequence: an unauthenticated nearby BLE client could write a
KISS frame and cause the tracker to transmit under the configured amateur-radio callsign.

## Changes

### 1. Project-owned, thread-safe runtime logging

- Replace the `peterus/esp-logger` dependency with a small project-owned logger retaining the
  existing `logging::Logger` calling style and optional UDP syslog methods.
- Format each complete record before acquiring the output mutex, using `va_copy()` correctly
  for the two `vsnprintf()` passes.
- Serialize final writes across FreeRTOS tasks and emit one plain-text record containing level,
  module, milliseconds, task name, and core number.
- Add GCC `printf` format checking to catch mismatched `%s` and Arduino `String` arguments.
- Format into a fixed-size stack buffer rather than a per-call heap allocation, since this runs
  on every DEBUG-level trace in the `loop()` hot path.
- Add a small `EarlySerialInit` global, declared before `Configuration Config;` in
  `LoRa_APRS_Tracker.cpp`, whose only job is `Serial.begin()`. `Config`'s constructor logs while
  loading SPIFFS, and C++ constructs globals in one file in declaration order, so this makes
  `Serial` genuinely ready before that first log call — otherwise (confirmed by reading the
  Arduino-ESP32 core) writing to `Serial` before `Serial.begin()` is a driver-level no-op on this
  core, independent of the logger's own state.
- Replace remaining active direct `Serial.print*()` diagnostics with the shared logger.
- Add a persisted, validated serial log level: Error (3), Warn (4), Info (6), or Debug (7).
- Expose that setting in Web Configuration; it takes effect after the normal save/reboot.
- Keep generated monitor logs local and ignored because they can contain callsigns and position
  packets.

### 2. Radio and web-configuration diagnostics

- Log RSSI, SNR, and frequency error at DEBUG immediately after a valid LoRa packet is decoded.
- Label the four default LoRa slots in the web UI as EU/WORLD, POLAND, UK, and US, matching the
  firmware's index-to-region mapping.
- Add `GET /capabilities`, populated from the same compile-time feature macros used by the
  firmware.
- Add generic `data-requires-capability` and fallback handling to the shared web UI.
- Hide the BLE/Classic selector on boards without `HAS_BT_CLASSIC` and show a BLE-only note.
- Enforce the same invariant in the POST handler so a handcrafted request cannot save Classic
  Bluetooth on hardware that lacks it.

Only the confirmed broken Bluetooth case is wired to the capability mechanism in this change.
Battery and GPS controls that are merely inert on some variants are deliberately left alone.

### 3. Authenticated persistent BLE bonding

- Require encrypted and authenticated access on both BLE TNC characteristics.
- Request bonding with MITM protection and bidirectional encryption/identity key distribution.
- Start security when a central connects and log GAP status text plus encryption,
  authentication, bonding, key size, peer identity, and stored-bond diagnostics.
- Remove delays from NimBLE callbacks.
- Add **Device -> Clear BLE bonds** to Web Configuration. It writes a one-shot SPIFFS marker,
  exits Web Configuration mode, and reboots. BLE setup deletes all NimBLE bonds and then removes
  the marker, preventing repeated deletion.

The implementation uses authenticated Legacy Passkey pairing (`bonding=true`, `mitm=true`,
`secureConnections=false`). Secure Connections was attempted first, but NimBLE-Arduino 1.4.1
and the tested Android client stalled and timed out before encryption completed. Legacy pairing
with explicit ENC+ID distribution produced a durable authenticated bond. This is a compatibility
tradeoff: a captured initial legacy exchange is weaker than LE Secure Connections. A future
NimBLE 2.x migration should retest Secure Connections as a separate change.

### 4. Random on-screen pairing passkey

- Generate a new value from 100000 through 999999 with the ESP32 hardware RNG for every physical
  BLE connection. Bonded peers restore saved keys and do not use or display this value.
- Preserve NimBLE-Arduino 1.4.1's internal `123456` callback sentinel, then return the actual
  random value from `onPassKeyRequest()`. This is necessary because that library version skips
  the display callback when a non-sentinel static passkey is configured.
- Pass the requested PIN from `nimble_host` to the Arduino loop with atomic integer state.
  NimBLE callbacks never perform display/I2C work, allocate Arduino strings, or delay.
- Wake an OLED disabled by eco mode and show a non-wrapping screen:

  ```text
  BLE PAIR
  Enter PIN on phone
  PIN: NNNNNN
  ```

- Refresh the pairing screen once per second so the normal UI cannot overwrite it, then clear it
  on authentication completion, disconnect, or a 60-second UI timeout.
- Log the existence of a PIN request at INFO. Log the PIN digits only at DEBUG because they are
  temporary authentication material.

## Investigation history and rejected approaches

- Encrypted characteristic flags without an explicit security request did not reliably trigger
  pairing in the tested client flow.
- An early unsolicited security request combined with unauthenticated/Just Works settings made
  the Android link unstable.
- Secure Connections plus a fixed display passkey requested a PIN but stalled and timed out in
  NimBLE-Arduino 1.4.1.
- The first random-PIN implementation configured the random value directly. NimBLE used it but
  skipped `onPassKeyRequest()`, so the tracker could not display it. Entering `123456` failed
  safely with confirm-value mismatch and no bond. Inspecting `NimBLEServer.cpp` exposed its
  sentinel behavior; routing the random value through the callback fixed the issue.
- `getNumBonds()` can still be zero inside the first authentication-complete callback because
  NimBLE persists the bond afterward. Later reconnect and reboot tests reported one stored bond.
- A pre-submission review found the logger's `ready_`/`begin()` gate dropped every message
  logged before `setup()` called `logger.begin()`, including everything `Configuration` logs
  from its global constructor. Removing the gate wasn't enough on its own: writing to `Serial`
  before `Serial.begin()` is itself a no-op at the Arduino-ESP32 driver level (confirmed by
  reading `HWCDC.cpp`/`HardwareSerial.cpp`), so `EarlySerialInit` was added to run
  `Serial.begin()` from a global constructed before `Config`, using C++'s same-file
  declaration-order guarantee. A third, separate cause of missing early boot output remains and
  is out of scope for this PR - see "Known limitations" below.

## Verification performed

### Build

The final pre-consolidation tree builds successfully with the project's global `-Wall -Werror`
for five representative environments:

| PlatformIO environment | Chip/display role | RAM | Application flash |
| --- | --- | ---: | ---: |
| `ttgo_t_beam_s3_SUPREME_v3` | ESP32-S3, primary tested OLED target | 55,492 / 327,680 (16.9%) | 1,416,953 / 3,145,728 (45.0%) |
| `ttgo-t-beam-v1_2` | Original ESP32 with Classic Bluetooth | 68,248 / 1,310,720 (5.2%) | 2,137,793 / 3,145,728 (68.0%) |
| `esp32_c3_DIY_LoRa_GPS` | ESP32-C3 BLE-only target | 49,324 / 327,680 (15.1%) | 1,469,752 / 3,145,728 (46.7%) |
| `heltec_wifi_lora_32_v3_2_TNC` | ESP32-S3 TNC/no-GPS variant | 55,360 / 327,680 (16.9%) | 1,403,141 / 3,145,728 (44.6%) |
| `ttgo_t_deck_plus` | ESP32-S3 TFT target, Arduino-ESP32 2.0.9 | 56,516 / 327,680 (17.2%) | 1,425,849 / 3,145,728 (45.3%) |

These are compile/link/size checks. Only the T-Beam Supreme v3 received the end-to-end hardware
tests below.

### Hardware

- Target: LilyGO T-Beam Supreme v3.
- Android requested a random PIN shown on the tracker; `118282` completed with encryption,
  authentication, bonding, and 16-byte key size.
- The peer disconnected and reconnected under its resolved identity without another PIN.
  The tracker then reported one stored bond.
- A position and APRS message traveled APRSdroid -> BLE KISS -> LoRa.
- The second tracker returned an APRS ACK via LoRa -> BLE -> APRSdroid.
- A firmware upload/reset preserved NVS. The saved peer reconnected silently and transmitted
  another position, proving bond persistence across reset.
- After clearing both sides, a second clean pairing used a different random PIN (`748043`) and
  again completed with encryption, authentication, bonding, and 16-byte key size.
- The shortened pairing header corrected the initial 128-pixel OLED line wrap.
- The Web Configuration bond-reset action successfully cleared the tracker before fresh pairing.

Raw logs are intentionally not proposed for the upstream commit because they contain station and
packet data. Sanitized excerpts can be supplied during review if requested.

## Known limitations and deliberately deferred work

- Pairing remains authenticated BLE Legacy Pairing, not LE Secure Connections, pending a focused
  NimBLE-Arduino 2.x migration and compatibility test.
- BLE TNC2 mode remains unverified/non-working with the tested Android client; BLE KISS is the
  supported tested path here.
- The capability endpoint currently exposes only `hasBTClassic`; broader battery/GPS capability
  gating can be added independently.
- End-to-end hardware testing covered the T-Beam Supreme v3. Representative original ESP32,
  ESP32-C3, ESP32-S3 TNC, and TFT variants compile successfully, but still require
  maintainer/community runtime testing.
- ESP32-S3 native USB-CDC can emit boot logs before PlatformIO reconnects. Runtime logs and
  `log2file` capture work; buffering early boot output is outside this change. Concretely, this
  means even the earliest `Configuration`-constructor diagnostics this PR adds (SPIFFS mount/read
  status) usually still won't be visible in a fresh-reset capture on this board: the native-CDC
  driver queues writes made while no USB host is attached into a small ring buffer under a FIFO
  eviction policy, and `setup()`'s own later output fills and wraps that buffer well before a
  host can reattach after any reset. `EarlySerialInit` (above) fixes the two causes within this
  PR's control - the logger no longer gates on its own readiness, and `Serial.begin()` now runs
  before `Config`'s constructor logs anything - but doesn't and can't fix this third, independent
  USB re-enumeration timing constraint.
- No APRSdroid source was modified.

## Suggested reviewer order

1. `include/logger.h`, `src/logger.cpp`, and the log-level configuration path.
2. `GET /capabilities`, web capability gating, region labels, and POST invariants.
3. BLE security properties, authentication callbacks, and bond reset.
4. Random passkey generation and callback-to-loop display handoff.
5. Hardware evidence and security tradeoff above.

## Attribution and provenance

The original LoRa APRS Tracker project and existing source are Ricardo Guzman / CA2RXU's work.

Antonio Megia / EA4HXZ:

- identified the operational problems and target hardware;
- corrected the hardware identification to T-Beam Supreme v3;
- identified the exact NA7Q APRSdroid `stable` client and required that APRSdroid remain
  unchanged;
- chose priorities and approved the legacy-pairing compatibility tradeoff after failed Secure
  Connections tests;
- proposed a random PIN displayed by the tracker;
- operated the Android devices and two radio trackers, cleared bonds, entered PINs, sent packets,
  checked screens, reset hardware, and supplied/confirmed all hardware and RF evidence;
- remains the commit author, prospective pull-request author, and person responsible for the
  submitted contribution.

Claude Code assistance, reconstructed from the 2026-09-04 commit and session notes:

- repository/board-capability analysis;
- initial runtime log-level configuration;
- received-signal logging;
- region labels;
- the generic web capability mechanism and BLE-only UI/backend guard.

OpenAI Codex assistance during the 2026-09-05 session:

- audited the earlier work and NA7Q BLE backend;
- implemented the serialized project-owned logger and converted active direct serial diagnostics;
- instrumented NimBLE security and analyzed live hardware traces;
- implemented encrypted/authenticated BLE characteristics, key distribution, persistent bonding,
  bond recovery, hardware-random passkeys, thread-safe display handoff, and OLED layout repair;
- compiled/flashed the test builds under Antonio's direction and maintained the technical/test
  documentation.

Claude Code assistance during a 2026-09-05 pre-submission review session (after the Codex work
above, same day):

- ran a 15-finding correctness/consistency review of the Codex-authored branch against
  `origin/main`, documented in `PR_REVIEW_NOTES.md` with provenance, blast radius, and fix
  options for each finding, reviewed and decided by Antonio before any change was made;
- fixed the approved findings: the logger initialization-order bugs (including the
  `EarlySerialInit` global and its three-layered root-cause investigation, above and in
  "Investigation history"), a syslog log-level filter gap, single-BLE-connection enforcement, an
  unchecked compare-exchange in the pairing-PIN timeout path, a `Configuration`-backed bond-reset
  flag replacing a SPIFFS marker file, and the `hasBLE`-gated "Clear BLE bonds" UI/backend guard;
- compiled the representative build matrix and flashed/hardware-tested the result on the T-Beam
  Supreme v3, confirming persistent BLE bonding and clean boot after the changes;
- maintained `PLANS.md`'s backlog for the findings deliberately deferred rather than fixed here.

There is no raw Claude chat transcript in the repository. The Claude/Codex division above is a
best-effort reconstruction from Git history, `CLAUDE.md`, `PLANS.md`, and the retained Codex
conversation. It should not be represented as exact line-by-line authorship. AI tools are
disclosed as implementation/review assistance, not as human copyright holders or project
authors.

## Before opening the pull request

- Contact Ricardo and confirm whether he prefers this as one integration PR or split into
  logging/UI/BLE PRs.
- Rebase the clean branch on the then-current `origin/main`.
- Re-run the representative build matrix.
- Re-test the final rebased firmware on the T-Beam Supreme if code conflicts changed behavior.
- Remove any internal draft/session files from the upstream diff.
- Do not include raw logs, private network addresses, serial ports, peer addresses, or precise
  packet/location data.
