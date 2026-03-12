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

## Phase 3: Fan State Machine

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
./build.sh <SSID> <PASS> && ./flash.sh && ./monitor.sh
```

**Test 3.1 — Identical behavior to Phase 2:**
- [ ] Repeat ALL Phase 2 button tests (2.2 through 2.10) — behavior must be identical
- [ ] The only difference should be in serial log format (now shows state machine transitions)

**Test 3.2 — State machine logging:**
- [ ] Each button press shows: received command → state transition → driver output
- [ ] Example log: `fan_control: cmd=SPEED_CYCLE src=BUTTON | state: running=1 speed=50 dir=EXHAUST`
- [ ] State transitions are logged even when state doesn't change (e.g., direction toggle when already at target)

**Test 3.3 — Command queue stress:**
- [ ] Rapidly press Speed button 10 times in ~2 seconds → all 10 commands processed in order
- [ ] No "queue full" warnings in serial log
- [ ] Final state is correct (10 presses from 25%: cycles 50→75→100→25→50→75→100→25→50→75)

**Test 3.4 — Concurrent button presses:**
- [ ] Press Speed and Direction at the same time → both commands processed (order may vary), no crash
- [ ] Fan ends up at expected state (speed changed AND direction changed)

**Test 3.5 — State consistency:**
- [ ] After any sequence of button presses, the serial-logged state always matches observed fan behavior
- [ ] `fan_control_get_state()` output (logged periodically) matches actual motor state

**Test 3.6 — Binary size:**
- [ ] Build output shows binary size < 1.5MB (record: ______ KB)

---

## Phase 4: WiFi + REST API + SSE

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
./build.sh <SSID> <PASS> && ./flash.sh && ./monitor.sh
```

**Test 4.1 — WiFi connection:**
- [ ] Serial shows "wifi: connecting to <SSID>..."
- [ ] Serial shows "wifi: got IP <address>" within ~5 seconds
- [ ] Serial shows "mdns: hostname set to vanfan.local"

**Test 4.2 — mDNS discovery:**
- [ ] From Mac/Pi: `ping vanfan.local` responds
- [ ] Or: `dns-sd -B _http._tcp` shows "VanFan Controller" service

**Test 4.3 — GET /api/v1/status:**
```bash
curl -s http://vanfan.local/api/v1/status | python3 -m json.tool
```
- [ ] Returns valid JSON: `{"running": false, "speed": 25, "direction": "exhaust", "mode": "manual"}`
- [ ] Turn fan on via button, repeat curl → `"running": true` with correct speed/direction

**Test 4.4 — POST /api/v1/speed:**
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"speed": 75}' http://vanfan.local/api/v1/speed
```
- [ ] Returns updated state with `"speed": 75`
- [ ] Fan physically changes speed (ramps to 75%)
- [ ] Invalid speed: `curl -d '{"speed": 150}'` → 422 error response

**Test 4.5 — POST /api/v1/direction:**
```bash
curl -s -X POST -H 'Content-Type: application/json' -d '{"direction": "intake"}' http://vanfan.local/api/v1/direction
```
- [ ] Fan ramps down, reverses, ramps back up
- [ ] Returns state with `"direction": "intake"`
- [ ] Invalid: `curl -d '{"direction": "sideways"}'` → 422 error

**Test 4.6 — POST /api/v1/set (combined endpoint):**
```bash
curl -s -X POST -H 'Content-Type: application/json' \
  -d '{"speed": 50, "direction": "exhaust"}' \
  http://vanfan.local/api/v1/set
```
- [ ] Both speed and direction updated atomically
- [ ] Fan turns on if it was off
- [ ] Partial update works: `curl -d '{"speed": 80}'` only changes speed
- [ ] Empty object `{}` returns current state without changes

**Test 4.7 — POST /api/v1/toggle:**
```bash
curl -s -X POST http://vanfan.local/api/v1/toggle
```
- [ ] Fan toggles on/off
- [ ] Returns updated state with correct `"running"` value

**Test 4.8 — POST /api/v1/stop:**
```bash
curl -s -X POST http://vanfan.local/api/v1/stop
```
- [ ] Fan stops immediately (emergency stop — no ramp or fastest possible ramp)
- [ ] Returns state with `"running": false`

**Test 4.9 — GET /api/v1/events (SSE):**
```bash
curl -N http://vanfan.local/api/v1/events
```
- [ ] Immediately receives initial state event: `data: {"running":false,"speed":25,...}`
- [ ] Press a button → new event appears in curl output within 100ms
- [ ] Send API command from another terminal → event appears in SSE stream
- [ ] Event includes `"source": "button"` or `"source": "api"` correctly
- [ ] Ctrl+C to disconnect. Reconnect → get fresh initial state.

**Test 4.10 — Multiple SSE clients:**
- [ ] Open 2 curl SSE connections simultaneously
- [ ] Press button → both clients receive the event
- [ ] Close one client → other continues receiving events

**Test 4.11 — GET /api/v1/info:**
```bash
curl -s http://vanfan.local/api/v1/info | python3 -m json.tool
```
- [ ] Returns JSON with: `version`, `uptime_s`, `free_heap`, `chip` fields
- [ ] Version matches `version.txt`
- [ ] Uptime increases on repeated calls
- [ ] Free heap is reasonable (~300-380KB)

**Test 4.12 — Buttons still work alongside API:**
- [ ] While SSE stream open: press buttons → events appear in stream
- [ ] While API is being used: buttons still respond immediately
- [ ] Rapid alternation between button presses and API calls → no crashes, state consistent

**Test 4.13 — WiFi resilience:**
- [ ] Turn off WiFi router. Fan keeps running. Buttons still work.
- [ ] Serial shows reconnection attempts with increasing backoff
- [ ] Turn router back on → ESP32 reconnects → API works again
- [ ] SSE clients that disconnected can reconnect

**Test 4.14 — Error handling:**
```bash
# Bad JSON
curl -s -X POST -H 'Content-Type: application/json' -d 'not json' http://vanfan.local/api/v1/speed
# Missing field
curl -s -X POST -H 'Content-Type: application/json' -d '{}' http://vanfan.local/api/v1/speed
# Wrong endpoint
curl -s http://vanfan.local/api/v1/nonexistent
```
- [ ] Bad JSON → 400 response
- [ ] Missing required field → 400 or 422 response
- [ ] Unknown endpoint → 404 response
- [ ] None of these crash the device

**Test 4.15 — Binary size:**
- [ ] Build output shows binary size < 1.5MB (record: ______ KB)
- [ ] WiFi + HTTP stack adds significant size — verify still under limit

---

## Phase 5: OTA

**Goal:** Remote firmware update from Pi with rollback safety.

### `components/ota/`
- `ota_init()` — called early in boot. If running from a pending-verify partition, validates (WiFi connected, motor driver init OK) then calls `esp_ota_mark_app_valid_cancel_rollback()`. If validation fails, bootloader rolls back on next reset.
- `ota_handle_upload(req)` — streaming upload handler:
  1. Stops fan (safety)
  2. `esp_ota_begin()` on next update partition
  3. Streams request body in 1KB chunks via `esp_ota_write()`
  4. `esp_ota_end()` validates image
  5. `esp_ota_set_boot_partition()` + respond 200 + `esp_restart()`

### Phase 5 Hardware Tests

**STOP here. Run all tests before proceeding to Phase 6.**

**Test 5.1 — Preparation:**
- [ ] Note current version from `curl http://vanfan.local/api/v1/info` → version "X"
- [ ] Note current OTA partition from info response or serial log

**Test 5.2 — Bump version and rebuild:**
```bash
echo "0.2.0" > version.txt
./build.sh <SSID> <PASS>
```
- [ ] New binary built successfully

**Test 5.3 — OTA upload:**
```bash
# Fan running at some speed
curl -X POST --data-binary @firmware/vanfan.bin http://vanfan.local/api/v1/ota/update
```
- [ ] Fan stops before OTA begins (safety)
- [ ] Serial shows OTA progress (bytes written, percentage)
- [ ] Response indicates success before restart
- [ ] Device reboots automatically

**Test 5.4 — Post-OTA verification:**
- [ ] Device reconnects to WiFi after reboot
- [ ] `curl http://vanfan.local/api/v1/info` shows version "0.2.0"
- [ ] Info shows different OTA partition than before (ota_0 ↔ ota_1)
- [ ] All functionality works: buttons, API, SSE

**Test 5.5 — Rollback (if possible to test):**
- [ ] Build a deliberately broken firmware (e.g., skip `esp_ota_mark_app_valid_cancel_rollback()`)
- [ ] Upload via OTA → device reboots → fails validation → reboots again → rolls back to previous working version
- [ ] Or: verify rollback API if implemented

**Test 5.6 — OTA rejection:**
- [ ] Upload garbage data: `echo "not firmware" | curl -X POST --data-binary @- http://vanfan.local/api/v1/ota/update`
- [ ] `esp_ota_end()` fails → error response → device does NOT reboot
- [ ] Fan and API continue working normally after rejected upload

**Test 5.7 — Binary size:**
- [ ] Record: ______ KB (OTA component shouldn't add much)

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
./build.sh <WIFI_SSID> <WIFI_PASSWORD>   # Build in Docker
./flash.sh                                 # Flash via USB
./monitor.sh                               # Serial monitor
```

After Phase 5, OTA replaces flash.sh for deployed unit:
```bash
curl -X POST http://vanfan.local/api/v1/ota/update --data-binary @firmware/vanfan.bin
```
