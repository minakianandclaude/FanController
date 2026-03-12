# VanFan Controller — Implementation Plan

## Context
ESP32-S3 based roof fan controller for a Sprinter camper van. Controls a DC fan via BTS7960 motor driver. Exposes REST API + SSE for Raspberry Pi integration. Physical 2-button control with OTA updates. Built on ESP-IDF v5.2.3 (not Arduino).

**Key decisions:**
- 2 buttons (speed + direction). Press-to-turn-on, hold-to-turn-off on either button
- Speed cycles 20→40→60→80→100→20. Turn-on resumes last used speed/direction
- No safety watchdog/auto-brake timeout — fan runs indefinitely once turned on (safety: dogs may be in van)
- Hardcoded WiFi credentials (compile-time injection via Docker, same as VOC project)
- BLE WiFi provisioning via RPi as stretch goal in Phase 6

**Development workflow:**
- Build in Docker container (`espressif/idf:v5.2.3`)
- After each phase: run `./build.sh` to verify clean build and check binary size (<1.5MB)
- **STOP at end of each phase** for manual hardware testing before proceeding

---

## Button Behavior (2-button design)

Two buttons: **Speed** and **Direction**. No dedicated on/off button.

| Button | Event | Fan OFF | Fan ON |
|--------|-------|---------|--------|
| Speed | PRESS | TURN_ON (resume last speed/dir) | SPEED_CYCLE (20→40→60→80→100→20) |
| Speed | HOLD | TURN_ON at 20% **intake** | TURN_OFF |
| Direction | PRESS | TURN_ON (resume last speed/dir) | DIRECTION_TOGGLE |
| Direction | HOLD | TURN_ON at 20% **exhaust** | TURN_OFF |

Hold-when-off rationale: quick-start shortcuts — speed hold = intake (pull air in quietly), direction hold = exhaust (vent out). Press-when-off always resumes last operating mode.

### Ramping Rules
- All speed changes ramp smoothly with asymmetric rates: 20ms/1% up, 50ms/1% down
- Direction flip: ramp down to 0, coast 2s (EN pins LOW, motor spins down freely), ramp up to target speed
- Turn off: ramp to 0 then disable

---

## Phase 1: Project Scaffold — COMPLETE

---

## Phase 2: BTS7960 Driver + Buttons — COMPLETE

**Goal:** Low-level motor control and button input working independently. No state machine yet — buttons call driver directly via simple glue in main.c.

### `components/bts7960/`

**Public API:**
- `bts7960_init(config)` — configure 2 LEDC channels (RPWM/LPWM) at 25kHz, 8-bit resolution; configure R_EN/L_EN as GPIO output (driven HIGH on init)
- `bts7960_set_output(int8_t speed_percent)` — -100 to +100, sign=direction. Drives one PWM channel while zeroing the other. **Soft-start ramp** via `esp_timer` one-shot callbacks with variable rate. New calls mid-ramp update the target seamlessly.
- `bts7960_brake()` — both PWM low, both EN high (active braking)
- `bts7960_coast()` — both EN low (free spin)
- `bts7960_read_current(channel)` — stub returning 0.0f (Phase 6)
- `bts7960_get_current_output()` — returns current actual setpoint (may lag target during ramp)

**Key details:**
- LEDC: `LEDC_LOW_SPEED_MODE`, `LEDC_TIMER_8_BIT`, 25kHz
- Ramp uses mutex-protected `current_output` and `target_output`
- Ramp via `esp_timer` one-shot callbacks (re-scheduled each step for variable rate)
- Asymmetric ramp rates: **20ms/1% ramp up** (2s full), **50ms/1% ramp down** (5s full)
- Direction switch: ramps through 0, then **coasts for 2s** (EN pins LOW, motor spins down freely) before ramping in new direction — avoids braking effect from driving against a still-spinning motor
- New `set_output()` mid-ramp just updates target — timer chases it

**Pin config (Kconfig):** RPWM=GPIO5, LPWM=GPIO6, R_EN=GPIO7, L_EN=GPIO15, R_IS=GPIO4, L_IS=GPIO3

### `components/buttons/`

**Public API:**
- `buttons_init(config)` — configure 2 GPIO inputs with internal pull-up, start polling task
- `buttons_register_callback(cb, user_data)` — register event callback

**Events:** `BTN_EVT_PRESS` (short press) and `BTN_EVT_HOLD` (held past 800ms)

**Implementation:** Polling task (4096 stack, priority 5) at 10ms intervals. Per-button state machine:
```
IDLE → (pin low) → DEBOUNCING → (stable 50ms) → PRESSED
PRESSED → (released < 800ms) → fire PRESS → WAIT_RELEASE
PRESSED → (held ≥ 800ms) → fire HOLD → WAIT_RELEASE
WAIT_RELEASE → (still pressed/bouncing) → reset release timer
WAIT_RELEASE → (stable released 50ms) → IDLE
```
WAIT_RELEASE ensures no spurious events from contact bounce on release — button is locked out until a clean debounced release is confirmed.

### `main/main.c` — temporary glue
- Init bts7960 + buttons with Kconfig pin values
- Button callback implements 2-button behavior table directly (moves to fan_control in Phase 3)
- Track simple state: `running`, `speed` (20/40/60/80/100), `direction` (+1/-1)
- Log every action to serial

### Changes from original plan
- **5 speed steps** (20/40/60/80/100) instead of 4 (25/50/75/100)
- **Asymmetric ramp rates**: 20ms/1% up, 50ms/1% down (originally 10ms/1% uniform)
- **Direction coast pause**: 2s coast at zero with EN disabled during direction flip
- **Button task stack**: 4096 bytes (originally 2048 — too small for callback chain with ESP_LOGI)
- **Hold threshold**: 800ms (originally 1s)
- **WAIT_RELEASE state**: replaced HELD state to prevent hold-release bounce causing spurious events

### Phase 2 Hardware Tests — ALL PASSED

Binary size: **217KB** (well under 1.5MB limit)

---

## Phase 3: Fan State Machine — COMPLETE

**Goal:** Formalize the button logic into a proper FreeRTOS state machine. Buttons and (future) API both post commands to a queue. State machine is the single owner of motor control.

### `components/fan_control/`

**State:**
```c
typedef struct {
    bool running;
    int8_t speed_percent;        // 1-100
    fan_direction_t direction;   // EXHAUST (+1) or INTAKE (-1)
    fan_mode_t mode;             // MANUAL (AUTO stubbed)
} fan_state_t;
```

**Commands:** `TURN_ON`, `TURN_OFF`, `TOGGLE`, `SET_SPEED`, `SPEED_CYCLE`, `SET_DIRECTION`, `DIRECTION_TOGGLE`, `SET_COMBINED`, `EMERGENCY_STOP`, `SET_MODE` — each tagged with source (`BUTTON`, `API`, `STARTUP`)

**Architecture:** FreeRTOS task (priority 10, 4096 stack) with `xQueueReceive` on a 16-deep command queue.

**Key functions:**
- `fan_control_init()` — create task/queue, register button callback, init state `{running: false, speed: 20, direction: EXHAUST, mode: MANUAL}`
- `fan_control_send_command(cmd)` — `xQueueSend` (used by buttons and API)
- `fan_control_get_state(out)` — mutex-protected state copy for API reads
- `fan_control_register_state_cb(cb)` — state change notification (for event_emitter)
- `apply_state_to_driver()` — converts state to `bts7960_set_output(speed * direction)`

**Speed cycle logic:**
```c
if (speed < 40) speed = 40;
else if (speed < 60) speed = 60;
else if (speed < 80) speed = 80;
else if (speed < 100) speed = 100;
else speed = 20;  // wrap around
```

### Rewire main.c
- Remove direct bts7960 calls from button callback
- Button callback now calls `fan_control_send_command()` with appropriate command type
- Init order: `bts7960_init()` → `fan_control_init()` → `buttons_init()`

### Phase 3 Hardware Tests

**STOP here. Run all tests before proceeding to Phase 4.**

```bash
./build.sh && ./flash.sh && ./monitor.sh
```

**Test 3.1 — Identical behavior to Phase 2:**
- [x] Repeat ALL Phase 2 button tests (2.2 through 2.10) — behavior must be identical
- [x] The only difference should be in serial log format (now shows state machine transitions)

**Test 3.2 — State machine logging:**
- [x] Each button press shows: received command → state transition → driver output
- [x] Example log: `fan_control: cmd=SPEED_CYCLE src=BUTTON | state: running=1 speed=50 dir=EXHAUST`
- [x] State transitions are logged even when state doesn't change (e.g., direction toggle when already at target)

**Test 3.3 — Command queue stress:**
- [x] Rapidly press Speed button 10 times in ~2 seconds → all 10 commands processed in order
- [x] No "queue full" warnings in serial log
- [x] Final state is correct (10 presses from 25%: cycles 50→75→100→25→50→75→100→25→50→75)

**Test 3.4 — Concurrent button presses:**
- [x] Press Speed and Direction at the same time → both commands processed (order may vary), no crash
- [x] Fan ends up at expected state (speed changed AND direction changed)

**Test 3.5 — State consistency:**
- [x] After any sequence of button presses, the serial-logged state always matches observed fan behavior
- [x] `fan_control_get_state()` output (logged periodically) matches actual motor state

**Test 3.6 — Binary size:**
- [x] Build output shows binary size < 1.5MB (record: **218KB**)

### Phase 3 Hardware Tests — ALL PASSED

Binary size: **218KB** (up from 217KB in Phase 2)

---

## Phase 4: WiFi + REST API + SSE — COMPLETE (see git history for details)

**Goal:** Full network control and event streaming alongside physical buttons.

### `components/wifi/`
- STA mode, credentials from `CONFIG_VANFAN_WIFI_SSID` / `CONFIG_VANFAN_WIFI_PASSWORD` (injected at build time)
- mDNS: hostname "vanfan", service `_http._tcp` on port 80
- Auto-reconnect with exponential backoff (1s→2s→4s...60s max) — never gives up
- Non-blocking init — fan/buttons work before WiFi connects

### `components/event_emitter/`
- Maintains up to 4 SSE client FDs (static array, mutex-protected)
- `event_emitter_notify(state, source)` — serializes state to SSE format (`data: {...}\n\n`), pushes to all connected clients via `httpd_socket_send()`
- Failed sends auto-remove dead clients
- 15-second keepalive comments to prevent timeout

### `components/api/`

REST endpoints (all JSON):
- `GET /api/v1/status` — current fan state
- `POST /api/v1/speed` — `{"speed": 75}` (1-100)
- `POST /api/v1/direction` — `{"direction": "intake"|"exhaust"}`
- `POST /api/v1/mode` — `{"mode": "manual"}`
- `POST /api/v1/set` — **combined endpoint**: `{"speed": 75, "direction": "exhaust", "mode": "manual"}` — all fields optional, only provided fields are changed. Applies atomically as a single state update via `FAN_CMD_SET_COMBINED`. Turns fan on if off.
- `POST /api/v1/toggle` — toggle on/off
- `POST /api/v1/stop` — emergency stop
- `GET /api/v1/events` — SSE stream (sends initial state on connect, then updates)
- `POST /api/v1/ota/update` — stub (Phase 5)
- `GET /api/v1/info` — version, uptime, heap, chip info

JSON parsing via cJSON (ESP-IDF component `json`). POST endpoints validate input, send command to fan_control queue, return updated state. 400 for bad JSON, 422 for out-of-range values.

### `main/main.c` init order:
1. NVS init
2. Chip info log
3. `bts7960_init()` — motor driver hardware
4. `buttons_init()` — start button polling
5. `fan_control_init()` — state machine + button callback
6. `event_emitter_init()` — register state change callback
7. `wifi_manager_init()` — non-blocking WiFi connect
8. `api_init()` — HTTP server

Rationale: hardware and physical controls before network, so buttons work immediately even if WiFi never connects.

### Phase 4 Hardware Tests

**STOP here. Run all tests before proceeding to Phase 5.**

```bash
./build.sh && ./flash.sh && ./monitor.sh
```

**Test 4.1 — WiFi connection:**
- [x] Serial shows "wifi: connecting to <SSID>..."
- [x] Serial shows "wifi: got IP <address>" within ~5 seconds
- [x] Serial shows "mdns: hostname set to vanfan.local"

**Test 4.2 — mDNS discovery:**
- [x] From Mac: `ping vanfan.local` responds (192.168.50.148)
- [x] `dns-sd -B _http._tcp` shows "VanFan Controller" service

**Test 4.3 — GET /api/v1/status:**
```bash
curl -s http://vanfan.local/api/v1/status | python3 -m json.tool
```
- [x] Returns valid JSON: `{"running": false, "speed": 20, "direction": "exhaust", "mode": "manual"}`
- [x] Turn fan on via API, repeat curl → `"running": true` with correct speed/direction

**Test 4.4 — POST /api/v1/speed:**
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"speed": 75}' http://vanfan.local/api/v1/speed
```
- [x] Returns updated state with `"speed": 75`
- [x] Fan physically changes speed (ramps to 75%)
- [x] Invalid speed: `curl -d '{"speed": 150}'` → 422 error response

**Test 4.5 — POST /api/v1/direction:**
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"direction": "intake"}' http://vanfan.local/api/v1/direction
```
- [x] Fan ramps down, reverses, ramps back up
- [x] Returns state with `"direction": "intake"`
- [x] Invalid: `curl -d '{"direction": "sideways"}'` → 422 error

**Test 4.6 — POST /api/v1/set (combined endpoint):**
```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"speed": 50, "direction": "exhaust"}' \
  http://vanfan.local/api/v1/set
```
- [x] Both speed and direction updated atomically
- [x] Fan turns on if it was off
- [x] Partial update works: `curl -d '{"speed": 80}'` only changes speed
- [x] Empty object `{}` returns current state without changes

**Test 4.7 — POST /api/v1/toggle:**
```bash
curl -s -X POST http://vanfan.local/api/v1/toggle
```
- [x] Fan toggles on/off
- [x] Returns updated state with correct `"running"` value

**Test 4.8 — POST /api/v1/stop:**
```bash
curl -s -X POST http://vanfan.local/api/v1/stop
```
- [x] Fan stops immediately (emergency stop — no ramp or fastest possible ramp)
- [x] Returns state with `"running": false`

**Test 4.9 — GET /api/v1/events (SSE):**
```bash
curl -N http://vanfan.local/api/v1/events
```
- [x] Immediately receives initial state event: `data: {"running":false,"speed":20,...}`
- [x] Send API command from another terminal → event appears in SSE stream
- [x] Event includes `"source": "api"` correctly
- [x] Keepalive comments received every 15s
- [x] Reconnect → get fresh initial state

**Test 4.10 — Multiple SSE clients:**
- [x] Open 2 curl SSE connections simultaneously
- [x] Both clients receive the toggle event
- [x] Close one client → other continues receiving events

**Test 4.11 — GET /api/v1/info:**
```bash
curl -s http://vanfan.local/api/v1/info | python3 -m json.tool
```
- [x] Returns JSON with: `version`, `uptime_s`, `free_heap`, `chip` fields
- [x] Version matches `version.txt` (0.1.0)
- [x] Uptime increases on repeated calls
- [x] Free heap: ~253KB (lower than projected due to WiFi + HTTP stack)

**Test 4.12 — Buttons still work alongside API:**
- [ ] Requires manual button testing (not testable remotely)

**Test 4.13 — WiFi resilience:**
- [ ] Requires router power cycle (not testable remotely)

**Test 4.14 — Error handling:**
```bash
# Bad JSON
curl -s -X POST -H 'Content-Type: application/json' -d 'not json' http://vanfan.local/api/v1/speed
# Missing field
curl -s -X POST -H 'Content-Type: application/json' -d '{}' http://vanfan.local/api/v1/speed
# Wrong endpoint
curl -s http://vanfan.local/api/v1/nonexistent
```
- [x] Bad JSON → 400 response
- [x] Missing required field → 400 response
- [x] Unknown endpoint → 404 response
- [x] None of these crash the device

**Test 4.15 — Binary size:**
- [x] Build output shows binary size < 1.5MB (record: **867KB**)
- [x] WiFi + HTTP stack adds significant size (218KB → 867KB) — still well under limit

### Phase 4 Hardware Tests — PASSED (12/14 remote, 2 require manual testing)

Binary size: **867KB** (up from 218KB — WiFi + HTTP + mDNS + TLS stack)
Free heap at runtime: **~253KB**

### Changes from original plan
- **Init order adjusted**: fan_control before buttons (fan_control registers button callback internally)
- **SSE close handling**: dead clients detected via failed `httpd_socket_send()` instead of per-socket close callback (`httpd_sess_set_close_fn` not available in ESP-IDF v5.2.3)
- **API documentation**: added `docs/api.md` with full endpoint reference and Pi integration guide

---

## Phase 5: OTA — COMPLETE

**Goal:** Remote firmware update from Pi with rollback safety.

### `components/ota/`
- `ota_init()` — called at end of boot sequence. If running from a pending-verify partition (rollback enabled), calls `esp_ota_mark_app_valid_cancel_rollback()`. Logs running and next update partitions.
- `ota_handle_upload(req)` — streaming upload handler:
  1. API layer stops fan (emergency stop for safety)
  2. `esp_ota_begin()` on next update partition with `OTA_SIZE_UNKNOWN`
  3. Streams request body in 1KB heap-allocated chunks via `esp_ota_write()`
  4. `esp_ota_end()` validates image
  5. `esp_ota_set_boot_partition()` + respond 200 + `esp_restart()`

### `main/main.c` init order (updated):
1. NVS → 2. Motor driver → 3. Fan control → 4. Buttons → 5. Event emitter → 6. WiFi → 7. API → **8. OTA boot validation**

### Phase 5 Hardware Tests

**Test 5.1 — Preparation:**
- [x] Current version: v0.1.3 on ota_0

**Test 5.2 — Bump version and rebuild:**
- [x] v0.2.0 binary built (877KB)

**Test 5.3 — OTA upload:**
- [x] Fan stops before OTA begins (safety) — serial shows `EMERGENCY_STOP`
- [x] Serial shows OTA progress (10% through 100%, bytes written)
- [x] Response: `{"success":true,"message":"OTA update complete, restarting..."}`
- [x] Device reboots automatically

**Test 5.4 — Post-OTA verification:**
- [x] Device reconnects to WiFi after reboot
- [x] `curl http://vanfan.local/api/v1/info` shows version "0.2.0"
- [x] Info shows `ota_partition: ota_1` (was ota_0)
- [x] API works: status, toggle confirmed

**Test 5.5 — Rollback:**
- [ ] Deferred — rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) not enabled for initial release. `ota_init()` has the pending-verify check ready for when rollback is enabled.

**Test 5.6 — OTA rejection:**
- [x] Garbage data rejected: `{"error":500,"message":"OTA write failed"}`
- [x] Device does NOT reboot — stays on v0.2.0 ota_1
- [x] API continues working normally after rejected upload

**Test 5.7 — Binary size:**
- [x] Record: **877KB** (up 10KB from Phase 4's 867KB)

### Phase 5 Hardware Tests — PASSED (6/7, rollback deferred)

Binary size: **877KB** (up from 867KB — OTA component minimal overhead)

### Changes from original plan
- **Rollback deferred**: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` not enabled — requires bootloader + app to be built together, complicates OTA where bootloader can't be updated. `ota_init()` has the code ready for future enablement.
- **OTA buffer heap-allocated**: 1KB buffer moved from stack to heap to avoid httpd task stack overflow during `esp_ota_end()` image validation.
- **httpd stack increased to 8192**: Default 4096 too small for OTA image validation call chain.
- **FreeRTOS timer task stack increased to 4096**: Default 2048 overflowed during WiFi reconnect timer callback (`ESP_LOGI` + `esp_wifi_connect()`).
- **Version banner dynamic**: `main.c` now reads version from `esp_app_get_description()` instead of hardcoded string.
- **`/api/v1/info` includes `ota_partition`**: Shows current running partition label for OTA verification.

---

## Phase 6: Hardening

**Goal:** Persistence, safety, and robustness. Stretch: BLE provisioning.

### `components/settings/` — NVS persistence
- Stores: `last_speed`, `last_direction`, `boot_auto_start`, `hostname`
- Write debounce: 5-second `esp_timer`. Rapid changes buffered, written at most once per 5s
- Defaults (first boot): 25% speed, exhaust, auto-start off, hostname "vanfan"
- Integrated with fan_control state change callback

### Brownout handler
- `esp_register_shutdown_handler()` to persist fan state to NVS on reset
- `bts7960_brake()` to ensure motor stops safely

### Current sense (BTS7960 IS pins)
- ADC oneshot reads on R_IS/L_IS pins
- Stall/overload detection (threshold TBD with real hardware)

### Boot state restoration
- Load last speed/direction from NVS
- If `boot_auto_start` enabled, send `TURN_ON` command with `SRC_STARTUP`

### Stretch: BLE WiFi Provisioning
- Use ESP-IDF's `wifi_provisioning` component with BLE transport
- RPi acts as provisioning client (sends SSID/password over BLE)
- Store received credentials in NVS, replacing hardcoded defaults
- Fallback: if NVS has credentials, use those; otherwise use compile-time defaults

### Phase 6 Hardware Tests

**Test 6.1 — NVS write debounce:**
- [ ] Press Speed button rapidly 10 times (cycles through speeds)
- [ ] Serial log shows NVS writes: should see 2-3 writes max (not 10)
- [ ] Look for "settings: write debounced" or similar in logs

**Test 6.2 — Persistence across power cycle:**
- [ ] Set fan to 75% intake via buttons or API
- [ ] Turn fan off (hold button)
- [ ] Unplug USB. Wait 3 seconds. Replug.
- [ ] Press Speed button → fan starts at 75% intake (NOT default 25% exhaust)

**Test 6.3 — First-boot defaults (factory reset):**
- [ ] Erase NVS: flash with `esptool.py erase_region 0x9000 0x6000`, then reflash full firmware
- [ ] Press Speed button → starts at 25% exhaust (default)

**Test 6.4 — Boot auto-start:**
- [ ] Enable auto-start via API (if endpoint added) or NVS tool
- [ ] Power cycle → fan auto-starts at last speed/direction without button press
- [ ] Serial shows "fan_control: auto-start from settings"

**Test 6.5 — Current sense readings:**
- [ ] Fan running at various speeds → serial log shows ADC current values
- [ ] Current increases with speed (values are reasonable, not 0 or saturated)
- [ ] Record baseline values for future overcurrent threshold calibration

**Test 6.6 — Brownout simulation:**
- [ ] Fan running. Pull USB quickly (simulates power loss).
- [ ] Replug → check NVS has correct last state persisted

**Test 6.7 — (Stretch) BLE provisioning:**
- [ ] From RPi, discover BLE provisioning service
- [ ] Send new WiFi credentials over BLE
- [ ] ESP32 reconnects to new network
- [ ] Credentials persist across reboot

**Test 6.8 — Final binary size:**
- [ ] Record: ______ KB (must be < 1.5MB even with BLE stack if enabled)

---

## FreeRTOS Task Summary

| Task | Stack | Priority | Purpose |
|------|-------|----------|---------|
| button_poll | 2048 | 5 | Poll GPIO every 10ms, debounce, fire events |
| fan_control | 4096 | 10 | Command queue processing, state machine |
| main (app_main) | 8192 | 1 | Init sequence then returns |
| WiFi (system) | system | system | ESP-IDF internal |
| httpd (system) | default | 5 | ESP-IDF HTTP server |

## Component Dependency Graph

```
main.c
 ├── bts7960        (LEDC PWM, GPIO)
 ├── fan_control    (requires: bts7960, buttons, settings)
 ├── buttons        (GPIO, FreeRTOS timers)
 ├── wifi           (esp_wifi, esp_netif, mdns)
 ├── event_emitter  (requires: fan_control, esp_http_server)
 ├── api            (requires: esp_http_server, fan_control, event_emitter, ota, json)
 ├── ota            (esp_ota_ops, esp_http_server)
 └── settings       (nvs_flash)
```

## Build & Deploy

```bash
./build.sh                                 # Build in Docker (credentials from .env)
./flash.sh                                 # Flash via USB
./monitor.sh                               # Serial monitor
```

After Phase 5, OTA replaces flash.sh for deployed unit:
```bash
curl -X POST http://vanfan.local/api/v1/ota/update --data-binary @firmware/vanfan.bin
```
