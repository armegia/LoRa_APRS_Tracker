# Review notes: `pr/diagnostics-ble-bonding`

> Working document, not for the upstream PR. Reviewed the diff of `pr/diagnostics-ble-bonding`
> against `origin/main` (the real upstream base, `bfd531a`) — the same base/branch pairing
> `PULL_REQUEST_DRAFT.md` targets. Nothing in the repo has been changed based on these findings
> yet; this is the "before we touch anything" writeup. Annotate inline or add a status line under
> each finding as you work through them.

**Methodology note:** provenance below is based on diffing each touched file against
`origin/main` directly (`git diff origin/main pr/diagnostics-ble-bonding -- <file>`), not on
file-level "modified" status — a file listed as modified can still contain long untouched
stretches. `menu_utils.cpp` turned out to be byte-identical to upstream, which matters for
finding #10.

---

## Tier 1 — undermines the PR's own stated purpose

### 1. The new logger drops every log line emitted before `setup()` runs

**Mechanism:** `Logger::log()` and `Logger::writeRecord()` (`src/logger.cpp:120,176`) both gate
output on `ready_ && level <= level_`. `ready_` is only set true by `logger.begin()`, which is
the first line of `setup()`. But `Configuration Config;` is a **global object**, constructed
before `setup()` ever runs, and its constructor (`src/configuration.cpp:421`) itself calls
`logger.log(...)` — "SPIFFS mount failed; formatting", "SPIFFS format failed", "Configuration
not found; creating defaults", "Default configuration created", etc.

**Why it matters / blast radius:** These are precisely the messages you'd want if SPIFFS is
corrupt or a device is fresh out of the box — the PR's whole pitch is "no more reflashing to
get diagnostics," and this exact category of diagnostic is the one silently eaten. If SPIFFS
totally fails to mount/format, the *only* log line you'd get today ("SPIFFS format failed") is
dropped, so a dead-SPIFFS board just looks like it hangs with zero serial output.

**Provenance — introduced by this PR.** Confirmed via `git diff origin/main
pr/diagnostics-ble-bonding -- src/configuration.cpp`: upstream's `Configuration::writeFile()`
used plain `Serial.println(...)` with no gating at all — it always printed. This PR rewired
those calls onto the new `logger.log(...)` and introduced the `ready_` concept in the same
change. The bug is 100% new, a side effect of the logger rewrite.

**Fix options:**
- **A — loosen the gate for Serial specifically.** `writeRecord()` already checks `serial_ !=
  nullptr` separately; drop the `ready_` requirement there and default `level_` to a sane value
  (e.g. INFO) at construction, so pre-`begin()` calls print at whatever level is already
  configured by default. `begin()` then just means "the log level has now been loaded from
  Config," not "logging may now occur." Small, contained.
- **B — buffer-and-flush.** Queue log calls made before `ready_` in a small fixed-size static
  buffer, flush once `begin()` runs. Preserves exact level-filtering semantics but adds real
  complexity for a boot-only window.
- **C — restructure initialization order.** Make `Configuration` lazy (explicit `Config.load()`
  called from `setup()` after `logger.begin()`, instead of doing SPIFFS I/O in the constructor).
  Architecturally "correct," but bigger/riskier right before a PR submission.
- *My read: **A** is the right size for this PR.*

**Status:** Done, with a layered story uncovered by hardware testing rather than static reading
alone — full account in `PLANS.md`'s item-1 backlog entry. Three stacked causes, found and fixed
(or identified as out of scope) one at a time:
1. The logger's own `ready_`/`begin()` gate — fixed by removing it (`logger.cpp`, `logger.h`,
   `LoRa_APRS_Tracker.cpp`), restoring upstream's original permissive behavior.
2. `Serial.begin()` not yet called when `Config`'s constructor runs — writing to `Serial` before
   `begin()` is a silent no-op at the Arduino-ESP32 driver level itself (confirmed by reading
   `HWCDC.cpp`/`HardwareSerial.cpp`), independent of the logger. Fixed with a tiny global,
   `EarlySerialInit`, declared before `Config` in `LoRa_APRS_Tracker.cpp`, exploiting C++'s
   same-translation-unit declaration-order guarantee for global construction.
3. Even with 1-2 fixed, a fresh-reset `log2file` capture still doesn't show the earliest lines —
   traced to a third, genuinely separate and out-of-scope cause: the native-USB-CDC ring buffer
   evicts old data (FIFO) while no host is connected, which is always true for the first few
   seconds after any chip reset (the reset is what disconnects the USB host in the first place).
   `setup()` prints more than the buffer holds before a host can reattach, so the earliest lines
   are gone before any monitor exists to read them. This is the same limitation
   `PULL_REQUEST_DRAFT.md` already documents ("ESP32-S3 native USB-CDC can emit boot logs before
   PlatformIO reconnects") — not new, and not fixable by 1-2's initialization-order work. Only
   the still-deferred full `Config.load()` rework (moving the logging past where the ring buffer
   would otherwise already be full of later output) would fully close this.

Fixes 1-2 are verified correct by direct reading of the exact driver code paths involved, even
though a live capture couldn't visually confirm them end-to-end on this specific board/reset
path.

---

### 2. "Clear BLE bonds" silently no-ops — and lies about it — when BLE isn't the active mode

**Mechanism:** The web action handler for `clear-ble-bonds` (`src/web_utils.cpp:118-143`)
unconditionally writes the `/clear_ble_bonds` marker, flips off the WiFi AP, and reboots. That
marker is only ever read inside `BLE_Utils::setup()` (`src/ble_utils.cpp:268`), and `setup()`
only calls `BLE_Utils::setup()` when `bluetoothActive && Config.bluetooth.useBLE` is true.

**Why it matters / blast radius:** On any board with Bluetooth off, or set to Classic mode (an
original-ESP32 board with `HAS_BT_CLASSIC`), the button reboots the device, tells the user "BLE
bonds will be cleared after reboot," and does nothing — the marker sits there forever, silently
misleading anyone who later re-enables BLE into thinking old bonds are already gone. Same class
of bug this PR's own capability-gating mechanism (`/capabilities`, `data-requires-capability`)
exists to prevent, just not applied here.

**Provenance — introduced by this PR.** "Clear BLE bonds" doesn't exist upstream; the entire
handler block is new.

**Fix options:**
- **A — backend guard (required regardless of B).** `if (!(bluetoothActive &&
  Config.bluetooth.useBLE)) { request->send(400, ...); return; }` — mirrors the exact condition
  `setup()` uses; matches CLAUDE.md's own "mirror capability guards into the POST handler"
  pattern.
- **B — hide it in the UI too**, via the same `data-requires-capability` mechanism this PR
  already built. Consistent, but additional surface area.
- *My read: **A is mandatory** (it's the actual bug); B is optional polish.*

**Status:** Done — you picked B; implemented A too and flagged it, since B alone leaves a raw
POST able to hit the same bug (CLAUDE.md's own defense-in-depth rule), and A is the one-line fix
that closes the actual reported issue. `web_utils.cpp` (guard + new `hasBLE` capability),
`index.html` (`data-requires-capability="hasBLE"` on the menu item).

---

### 3. Syslog output ignores the configured log level entirely

**Mechanism:** `writeRecord()` (`src/logger.cpp:176-182`) checks `ready_ && level <= level_`
before writing to Serial, but the syslog branch right below only checks `syslogSet_` — no level
comparison at all.

**Why it matters / blast radius:** Currently dormant — nothing calls `setSyslogServer()` yet
(PLANS.md defers wiring it into the UI). But it's public API this PR ships, and the moment
anyone wires it up, `Config.logLevel = Error` on the web UI will still ship every DEBUG line —
including per-packet RSSI/SNR traces — over UDP, unfiltered.

**Provenance — introduced by this PR** (new file; the split Serial/syslog codepath and the
missing filter are both new).

**Fix options:**
- **A — one-line fix:** `if (syslogSet_ && level <= level_) { syslogLog(...); }`. Matches
  Serial's behavior exactly.
- **B — separate syslog threshold** (`syslogLevel_`) — more flexible, but a new persisted knob
  nobody has asked for on a feature not yet wired to the UI.
- *My read: **A**, revisit B when syslog UI wiring actually happens.*

**Status:** Done — A. `logger.cpp`.

---

## Tier 2 — real but narrower-window bugs

### 4. The pairing PIN is one shared value across all BLE connections

**Mechanism:** `pairingPasskey`/`pairingDisplayPasskey` (`src/ble_utils.cpp:57-58`) are single
global atomics, overwritten by `preparePairingPasskey()` on every `onConnect()`. NimBLE-Arduino's
default max simultaneous connections is 3.

**Why it matters / blast radius:** If a second BLE central connects while a first connection's
handshake is between `onPassKeyRequest()` (reads the passkey) and `publishPairingPasskey()`
(re-reads for display), the tracker's screen can show connection B's freshly generated PIN
while NimBLE already told connection A's phone to expect A's original value — a user-visible,
security-relevant mismatch (not a leak, just broken pairing UX at a bad moment).

**Provenance — introduced by this PR.** The entire secure-pairing mechanism is new; upstream's
`ble_utils.cpp` had no security at all — `onConnect()` just set a bool and logged "BLE Client
Connected".

**Fix options:**
- **A — key state per connection handle** (small fixed array/map keyed by `conn_handle`). The
  "textbook correct" fix, adds complexity for a scenario needing two centrals racing.
- **B — cap BLE to one connection:** `NimBLEDevice::setMaxConnections(1)`. The rest of this
  codebase already assumes a single BLE peer everywhere (`bluetoothConnected`,
  `shouldSendBLEtoLoRa`, `kissSerialBuffer` are all single-connection globals) — this closes the
  race by matching config to what the code already assumes, zero added complexity. Trade-off:
  permanently rules out >1 simultaneous BLE client (arguably already the intended design for a
  personal tracker).
- **C — leave as-is, documented.** Low real-world likelihood for this hardware's use case.
- *My read: **B**.*

**Status:** Done — B, adjusted: `NimBLEDevice::setMaxConnections()` doesn't exist in
NimBLE-Arduino 1.4.1 (this pin) — that's a 2.x-only API, confirmed the build failure and then
`grep`ped the vendored library source. Enforced the same one-connection-at-a-time intent at the
application layer instead: `onConnect()` now checks `pServer->getConnectedCount()` and
immediately `disconnect()`s anything beyond the first. `ble_utils.cpp`.

---

### 5. "Clear BLE bonds" button always shows success, even on failure

**Mechanism:** `data_embed/script.js:70-80`'s click handler fires the POST with no
`.then()`/`.catch()`, then immediately shows the success toast.

**Blast radius:** If the backend returns an error (its own paths return 500 on marker/config
write failure), the user sees success regardless — misleading, not destructive.

**Provenance — introduced by this PR** (button and handler don't exist upstream).

**Fix options:**
- **A:** `.then(r => showToast(r.ok ? "BLE bonds will be cleared when the device reboots" :
  "Could not schedule BLE bond reset")).catch(() => showToast("Could not reach the tracker"))`.
- **B:** Leave fire-and-forget but water down the copy to "Requesting BLE bond reset…". Weaker
  UX, zero code risk.
- *My read: **A**.*

**Status:** Done — A, after checking the reboot/AP-disable concern you raised. Traced the actual
C++ handler: on the two failure paths (marker/config write errors) it `return`s *before* ever
calling `ESP.restart()` — no reboot happens, so the response is never racing against anything on
those paths. On the success path, `request->send(200, ...)` is queued, then there's a `delay(500)`
before `ESP.restart()` — the identical pattern already used by the working config-save flow
("Save" button), which nothing here changes. So the fetch's response reliably arrives before the
device actually goes down; the fix is safe. One thing *not* to copy: the config-save flow's
`checkConnection()` helper (in `script.js`) retries `/status` until the device comes back up —
that pattern would be *wrong* here, because both this action and the save flow explicitly disable
`wifiAP.active` before rebooting (intentionally exiting Web Configuration mode back to normal
operation), so the device's own AP — and the client's connection to it — never comes back. That
retry loop already can't succeed for the Save button either; it's a pre-existing rough edge
elsewhere in the codebase, unrelated to this PR, not something to import into this fix.
`script.js`.

---

### 6. `Config.logLevel` cast to an enum without validation, unlike every other read site

**Mechanism:** `setup()` (`src/LoRa_APRS_Tracker.cpp:130`) does
`static_cast<logging::LoggerLevel::Value>(Config.logLevel)` directly. Both
`configuration.cpp`'s `readFile()` and the web POST handler validate with
`LoggerLevel::isValidValue()` first and fall back to INFO.

**Blast radius:** Only reachable if `Configuration::Configuration()` returns early because
SPIFFS mount **and** format both fail (`src/configuration.cpp:421-428`) — `setDefaultValues()`/
`readFile()` never run. `Config` is a file-scope global, so C++ guarantees `logLevel` is
zero-initialized in that case (not arbitrary garbage — worth being precise). But `0` isn't a
valid `LoggerLevel::Value` (enum starts at `ERROR = 3`), and `level <= level_` with
`level_ = 0` suppresses *everything*, including ERROR — at exactly the moment (total SPIFFS
failure) you'd most want ERROR-level output.

**Provenance — introduced by this PR.** The runtime `logLevel` field and this cast are new;
upstream had a compile-time `#define DEBUG` with no runtime cast anywhere.

**Fix options:**
- **A:** Mirror the existing pattern inline: validate-or-default-to-INFO at the call site.
- **B:** Move validation *into* `Logger::setDebugLevel()` itself, enforced once centrally
  instead of duplicated at 3 call sites. Fixes the "someone forgets to validate" root cause, at
  the cost of a slightly bigger diff.
- *My read: **B** if you're willing to touch `logger.h`/`.cpp` again; **A** for the smallest
  diff.*

**Status:** Done — B, confirmed this is the less-debt option, not backwards. `readFile()`'s and
the POST handler's validation serve a *different* purpose (detecting an invalid persisted value
to trigger `needsRewrite`/reject a bad save) and are untouched. The only thing moved is the
defensive check protecting the cast-and-use at the `setDebugLevel()` call site, which now lives
once inside the setter instead of needing to be remembered at every future call site — including
wherever a future item-1.C initialization-order change ends up calling it from. Free to do now
since `Logger` is new in this PR and has no other callers yet to break. `logger.cpp`.

---

### 7. Unchecked compare-exchange can log a spurious "pairing timed out"

**Mechanism:** `handlePairingDisplay()`'s timeout branch (`src/ble_utils.cpp:344`) calls
`pairingDisplayPasskey.compare_exchange_strong(passkey, 0)` but ignores its boolean return.

**Blast radius:** Purely cosmetic/logging — if a new passkey is published in the same instant
the old one's 60s timeout fires, the CAS fails, but the code still logs a WARN "timed out" and
resets local statics, skipping the still-valid new PIN for one loop iteration. No security
impact.

**Provenance — introduced by this PR** (the whole passkey-display state machine is new).

**Fix options:**
- **A:** Check the return value; only log/reset on success. One-line fix, no real trade-off.
- *My read: just do **A**.*

**Status:** Done — A. `ble_utils.cpp`.

---

### 8. New BLE pairing-PIN screen bypasses the existing menu display state machine

**Mechanism:** `LoRa_APRS_Tracker.cpp:236` introduces a standalone `blePairingDisplayActive`
bool gating two `MENU_Utils::showOnScreen()` call sites, instead of adding a state to the
existing `menuDisplay` state machine (`menu_utils.cpp`) that already owns "who gets the screen"
plus its own 30s idle-timeout.

**Blast radius:** If a user is mid-menu-navigation exactly when a phone pairs, the menu freezes
on the BLE PIN screen for up to 60s (menu's own timeout can't fire since it isn't being
called), and the next `showOnScreen()` afterward can abruptly reset whatever menu/message-compose
state was mid-flight. A third feature wanting the screen later means a third ad-hoc bool.

**Provenance — introduced by this PR**, layered on top of pre-existing, untouched
`menu_utils.cpp`.

**Fix options:**
- **A — full fix:** add a proper `menuDisplay` case for "showing BLE pairing PIN," reusing the
  existing state machine and timeout. Architecturally correct, but touches shared menu code
  broadly — more risk right before submitting a PR otherwise scoped to logging/BLE.
- **B — document and defer,** consistent with the PR draft's own "Known limitations and
  deliberately deferred work" section. Window is narrow and self-bounded (60s).
- *My read: **B** for this PR; flag as a `PLANS.md` backlog item.*

**Status:** Documented, no code change — B, with A put on hold for a future PR, same as #1.C.
Backlog item added to `PLANS.md`.

---

### 9. No on-device sign that a pairing/authorization attempt was rejected

**Mechanism:** TX/RX characteristics now require `READ_ENC`/`READ_AUTHEN`/`WRITE_ENC`/
`WRITE_AUTHEN` (`src/ble_utils.cpp:298`). A client that connects but never completes pairing has
every GATT access silently rejected at the ATT layer — nothing surfaces to the tracker's own
display.

**Blast radius:** UX only, not security/correctness — "silently deny" is the *safe* default
here. A user just sees "connected, nothing happens" with no clue why until checking serial.

**Provenance — introduced by this PR** (there was no concept of "rejected" before — nothing was
ever rejected).

**Fix options:**
- **A:** Reuse the pairing-display OLED slot to hint when a connected-but-unauthenticated peer
  is denied — needs a signal path from the ATT rejection up to the display loop that NimBLE's
  callbacks may not directly expose; nontrivial.
- **B:** Leave as a documented known limitation (fits the PR draft's existing section).
- *My read: **B** for this PR.*

**Status:** Documented, no code change — B. Noted in `PLANS.md`'s known-limitations backlog.

---

### 10. Turning Bluetooth off from the on-device menu can hide a PIN mid-pairing

**Mechanism:** The Bluetooth on/off menu toggle (`menu_utils.cpp:407-411`, confirmed
byte-identical to upstream, untouched by this PR) just flips the `bluetoothActive` bool — it
doesn't stop the BLE stack. The *new* pairing-display gate in `loop()` reads that same flag to
decide whether to call `BLE_Utils::handlePairingDisplay()` at all.

**Blast radius:** If a user toggles "Bluetooth OFF" from the menu while the (still actually
running) BLE stack is mid-pairing with a phone, `loop()` stops calling
`handlePairingDisplay()`, so a pending or already-shown PIN just vanishes — the phone waits for
a PIN entry the tracker will never display again for that session.

**Provenance — split.** The toggle itself is **pre-existing, unmodified** (verified `git diff`
on `menu_utils.cpp` is empty vs. upstream). The bug is a **new interaction** introduced because
this PR added a second consumer (`blePairingDisplayActive`) of that pre-existing flag without
accounting for what it means to BLE.

**Fix options:**
- **A — fix in pre-existing code:** have the menu's Bluetooth-off handler also actually tear
  down BLE (`BLE_Utils::stop()`), making the flag and reality match. Correct, but expands this
  PR into a file it currently has no other reason to touch.
- **B — fix in new code only:** in the `loop()` gate, don't let `bluetoothActive` toggling off
  suppress a pairing display already active — `handlePairingDisplay()` already returns `false`
  on its own once there's no live passkey.
- *My read: **B** — stays entirely inside code this PR already owns.*

**Status:** Done — B: the `loop()` gate no longer checks `bluetoothActive`, only
`Config.bluetooth.useBLE`, so `handlePairingDisplay()` keeps running (and clears itself
naturally) even if the menu toggle flips mid-pairing. `LoRa_APRS_Tracker.cpp`. A (making the
menu's Bluetooth-off handler actually call `BLE_Utils::stop()`) documented in `PLANS.md` as a
separate future PR, since it touches previously-untouched `menu_utils.cpp`.

---

### 11. Logged peer address may be a placeholder on a brand-new device's first connection

**Mechanism:** `onConnect()`'s new diagnostic (`src/ble_utils.cpp:144`) logs
`desc->peer_id_addr`, the *resolved identity* address — which for a first-time connection using
a private/random BLE address may not be resolved until pairing (async, later) completes.

**Blast radius:** Cosmetic — the one diagnostic added specifically to help debug pairing can
show a wrong/placeholder address during exactly the event (first-ever pairing) it's meant to
help diagnose.

**Provenance — introduced by this PR** (this log line is new).

**Fix options:**
- **A:** Also log `desc->peer_ota_addr` (the address actually used over the air, always valid at
  connect time) alongside `peer_id_addr`.
- **B:** Rely on the address already logged in `onAuthenticationComplete()`, where resolution is
  guaranteed complete, as the authoritative record.
- *My read: both are cheap; do **A**, note **B** already covers the authoritative case.*

**Status:** Done — A: `onConnect()` now logs both `peer_id_addr` and `peer_ota_addr`.
`ble_utils.cpp`.

---

## Tier 3 — maintainability / design consistency

### 12. Bond-reset marker path is a hand-copied string literal in a second file

**Mechanism:** `ble_utils.cpp:31` defines `#define BLE_BOND_RESET_FILE "/clear_ble_bonds"` and
uses the macro. `web_utils.cpp:119,134` instead hardcodes the literal twice.

**Blast radius:** If the path is ever renamed/moved, the compiler won't catch the drift — the
web handler keeps "successfully" scheduling a reset that `ble_utils.cpp`'s `SPIFFS.exists()`
check silently never observes.

**Provenance — introduced by this PR** (both the macro and the duplicate are new).

**Fix options:**
- **A:** Move the `#define` to `include/ble_utils.h` and `#include` it from `web_utils.cpp`.
- **B:** Expose a `BLE_Utils::scheduleBondReset()` function instead, so `web_utils.cpp` doesn't
  need to know it's a SPIFFS file at all. Nicer encapsulation, marginally larger diff.
- *My read: **A** for this PR.*

**Status:** Superseded by #13 — you chose B there (a real `Configuration` field instead of a
SPIFFS marker file), which removes the marker file and its path string entirely. There's no
longer a duplicated literal to share a macro for, so A here is moot rather than skipped.

---

### 13. Bond-reset uses a bespoke SPIFFS marker file instead of the `Configuration` persistence pattern

**Mechanism:** CLAUDE.md documents a specific four-places-in-lockstep pattern for anything that
needs to survive a reboot. The bond-reset flag instead uses a raw SPIFFS file written/checked/
deleted by hand across two files, alongside a separate direct `Config.writeFile()` call to flip
`wifiAP.active` — a second, parallel persistence path.

**Blast radius:** This marker gets none of `Configuration`'s safety net (no `needsRewrite`
corruption/migration handling). A future SPIFFS-cleanup or config-migration routine that only
knows about `/tracker_conf.json` won't account for this second stateful file.

**Provenance — introduced by this PR.**

**Fix options:**
- **A — leave as-is, comment it.** Working, narrow, one-shot; folding it into `Configuration`'s
  lockstep pattern touches 4 files for a boolean meaningful for at most one boot cycle. Add a
  short comment explaining the intentional divergence.
- **B — unify:** add `other.bleBondResetPending` to `Configuration` proper. More consistent,
  heavier change for low practical gain.
- *My read: **A** — a design note worth writing down, not necessarily a code change.*

**Status:** Done — B, per your call ("I want the new code to be right"). Added
`bool bondResetPending` to the `BLUETOOTH` config struct (`configuration.h`), wired through
`writeFile()`/`readFile()`/`setDefaultValues()` (`configuration.cpp`) with the same
`needsRewrite` treatment as every other field. The web action now sets
`Config.bluetooth.bondResetPending = true` and calls `Config.writeFile()` instead of touching
SPIFFS directly (`web_utils.cpp`); `BLE_Utils::setup()` checks and clears that field instead of
a marker file, and no longer needs `<SPIFFS.h>` at all (`ble_utils.cpp`).

---

### 14. Dead code: `getLineColor()`

**Mechanism:** Declared in `logger.h:47`, defined in `logger.cpp:40` returning `""`
unconditionally. Zero call sites anywhere in the repo — a vestige of the ANSI-color support
PLANS.md says was deliberately removed so captured logs stay plain text.

**Provenance — introduced by this PR** (new file; dead-on-arrival, not rot from later removal
elsewhere).

**Fix options:**
- **A:** Delete it (declaration + definition). Nothing references it; no functional risk.
- *My read: just **A**.*

**Status:** Done — A: declaration and definition removed. `logger.h`, `logger.cpp`.

---

### 15. Every log call heap-allocates, including in the packet-RX hot path

**Mechanism:** `Logger::log()` (`src/logger.cpp:118-153`) does a `vsnprintf` size probe, a `new
char[]` heap allocation, then a second `vsnprintf`, on every call that passes the level filter.

**Blast radius:** With `Config.logLevel` now settable to Debug from the web UI (this PR's own
feature), every LoRa RX/TX event (`lora_utils.cpp:262`'s new RSSI/SNR trace) allocates and frees
on the ESP32 heap once per received packet. Not catastrophic on ESP32's heap size, but it's the
textbook case embedded code avoids allocation in hot paths for (fragmentation risk under
sustained traffic).

**Provenance — introduced by this PR** (new file/implementation; can't compare directly against
the removed third-party `peterus/esp-logger`'s internals, but this specific two-pass-plus-heap
approach is this PR's own choice).

**Fix options:**
- **A:** Fixed-size stack buffer (`char buf[256]; vsnprintf(buf, sizeof(buf), ...)`), truncating
  on overflow. Standard embedded pattern, zero heap traffic, bounded stack cost; minor risk of
  truncating an unusually long message (none currently approach that length in this codebase).
- **B:** Leave it — ESP32 has a real heap, the DEBUG line in question is short and fires at most
  once per received packet (not a tight loop); reasonable to call the fragmentation risk low
  enough to accept.
- *My read: **A** — low-risk, and this is exactly the kind of hot-path allocation embedded
  conventions exist to avoid.*

**Status:** Done — A: single `vsnprintf` into a fixed 256-byte stack buffer, no heap allocation,
no size-probe pass. `logger.cpp`.

---

## Next step

Once you've annotated/decided, tell me which numbers to fix and which lettered option where I
gave more than one — I'll implement only those.
