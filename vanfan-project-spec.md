# VanFan Controller — Project Specification

## Purpose
ESP32-S3 based roof fan controller for a Mercedes Sprinter 144 camper van conversion. Controls a DC roof fan via BTS7960 motor driver. Exposes REST API + SSE event stream for integration with a Raspberry Pi central van computer. Includes physical button inputs and OTA firmware update capability.

## Hardware

### MCU: SparkFun Thing Plus ESP32-S3 (DEV-24408)
- Module: ESP32-S3-MINI1-N4R2
- Processor: Dual-core Xtensa LX7, 240 MHz
- Flash: 4MB Quad SPI (constrains OTA partition sizing — ~1.5MB per app slot)
- PSRAM: 2MB
- WiFi: 2.4 GHz 802.11 b/g/n, built-in PCB antenna
- BLE: Bluetooth 5 LE (not used in this project)
- USB: USB-C, native USB 2.0 OTG (no external UART bridge)
- GPIO: 21 broken out. Pins 10, 14, 15, 16, 17, 18 are ADC + touch capable.
- I2C: IO8 (SDA), IO9 (SCL) on Qwiic connector
- SPI: POCI, PICO, SCK on headers
- UART: RX/TX on headers
- SD: microSD via SDIO-4 (not used in this project)
- Battery: JST LiPo connector with MCP73831 charger + fuel gauge (not used)
- Status LED: IO0 (green), WS2812 RGB on IO46
- Peripheral power control: IO45 controls 3.3V_P rail via RT9080

### Motor Driver: BTS7960 Dual H-Bridge Module
- Amazon ASIN: B0BV925J5W
- IC: Two BTS7960 half-bridges (Infineon)
- Inputs: RPWM, LPWM (PWM speed/direction), R_EN, L_EN (enable pins)
- Current sense: IS pins on each half-bridge (analog output, usable for stall/overload detection)
- Voltage: Up to 27V motor supply
- Current: 43A continuous
- Logic level: 3.3V compatible (works directly with ESP32-S3 GPIO)
- Control scheme: RPWM drives one direction (exhaust), LPWM drives other direction (intake). EN pins held HIGH to enable. PWM duty cycle controls speed.

### Physical Buttons (3x, wired to GPIO)
- Speed cycle (cycles through preset speed levels or increments)
- Direction toggle (intake/exhaust)
- On/Off
- Debounce: hardware RC filter preferred, software debounce as backup (FreeRTOS task, ~50ms debounce window)

### Network Target
- Raspberry Pi (model TBD) running as central van computer
- Pi and ESP32-S3 on same local WiFi network (van router)
- Pi discovers ESP32 via mDNS (Avahi on Pi resolves `vanfan.local`)

## Software Platform
- Framework: ESP-IDF (NOT Arduino). Required for native partition table control, OTA, FreeRTOS primitives, LEDC PWM, and httpd.
- Target: esp32s3
- Build system: CMake (ESP-IDF standard)
- No Arduino libraries. No PlatformIO unless wrapping ESP-IDF.

## Architecture

### Partition Table (custom, defined in Phase 1)
```
# Name,    Type, SubType, Offset,  Size
nvs,       data, nvs,     0x9000,  0x6000
otadata,   data, ota,     0xf000,  0x2000
ota_0,     app,  ota_0,   0x20000, 0x180000
ota_1,     app,  ota_1,   0x1A0000,0x180000
```
Approximate: 1.5MB per OTA slot. Tight but sufficient — keep dependencies minimal, no SPIFFS, no web UI assets.

### Layer Architecture (bottom-up dependency)
```
[Physical Buttons] ──┐
                     ├──▶ [Fan State Machine] ──▶ [SSE Event Emitter]
[REST API Commands] ─┘         │
                               ▼
                        [BTS7960 Driver]
                               │
                               ▼
                        [LEDC PWM + GPIO]
```

### Component Breakdown

#### 1. BTS7960 Driver (`components/bts7960/`)
- Low-level hardware abstraction
- Configures two LEDC PWM channels (one for RPWM, one for LPWM)
- GPIO control for R_EN, L_EN
- Public API:
  - `esp_err_t bts7960_init(bts7960_config_t *config)` — pin assignments, PWM frequency (25kHz recommended for quiet operation)
  - `esp_err_t bts7960_set_output(int8_t speed_percent)` — -100 to +100, sign = direction, magnitude = duty cycle. 0 = brake/stop.
  - `esp_err_t bts7960_brake(void)` — active brake (both EN high, both PWM low)
  - `float bts7960_read_current(bts7960_channel_t channel)` — read IS pin via ADC for current sense (Phase 6)
- Soft-start ramp: internal ramp rate limiter, configurable ms-per-percent-step. Do not allow instantaneous 0→100% transitions.
- Safety: if no command received within configurable timeout (default 30s), auto-brake. Watchdog integration.

#### 2. Button Handler (`components/buttons/`)
- GPIO input config with internal pull-up/pull-down as needed
- Software debounce via FreeRTOS timer or dedicated task (50ms window)
- On validated press, posts command to fan state machine command queue
- Button-to-command mapping:
  - On/Off button → `FAN_CMD_TOGGLE`
  - Direction button → `FAN_CMD_DIRECTION_TOGGLE`
  - Speed button → `FAN_CMD_SPEED_CYCLE` (cycles through preset levels, e.g., 25/50/75/100%)
- GPIO pin assignments: TBD at wiring time, define in Kconfig or header

#### 3. Fan State Machine (`components/fan_control/`)
- Runs as a FreeRTOS task
- Receives commands via `xQueueReceive()` from both buttons and API handler
- Command types: `SET_SPEED`, `SET_DIRECTION`, `TOGGLE`, `DIRECTION_TOGGLE`, `SPEED_CYCLE`, `SET_MODE`, `EMERGENCY_STOP`
- Maintains canonical state:
  ```c
  typedef struct {
      bool running;
      int8_t speed_percent;    // 1-100 (magnitude only, direction separate)
      fan_direction_t direction; // FAN_DIR_INTAKE or FAN_DIR_EXHAUST
      fan_mode_t mode;          // FAN_MODE_MANUAL (FAN_MODE_AUTO stubbed for future temp sensor)
  } fan_state_t;
  ```
- On any state change: calls down to BTS7960 driver, then notifies event emitter
- Thread-safe state access via mutex for read from API handler
- Temperature sensor interface: stub `fan_control_set_temp_input()` for future integration. Do not implement auto mode logic yet.

#### 4. Event Emitter (`components/event_emitter/`)
- Maintains a list of connected SSE clients (single client expected but support up to 4)
- On state change notification from fan state machine, serializes state to JSON and pushes to all connected SSE streams
- JSON event format:
  ```json
  {"speed": 75, "direction": "exhaust", "running": true, "mode": "manual", "source": "button"}
  ```
- `source` field: `"button"`, `"api"`, `"timeout"`, `"startup"` — indicates what triggered the state change
- If client disconnects, clean up file descriptor. SSE auto-reconnects on client side (EventSource spec).

#### 5. WiFi + Network (`main/` or `components/wifi/`)
- STA mode only. No AP mode.
- WiFi credentials stored in NVS. Provisioned via serial/NVS tool initially (or add a provisioning command to the API).
- On connect: register mDNS service name `vanfan` → resolvable as `vanfan.local`
- mDNS implementation: `mdns_init()`, `mdns_hostname_set("vanfan")`, `mdns_instance_name_set("VanFan Controller")`. One-time setup at boot after WiFi connected.
- Reconnection: auto-reconnect on WiFi drop with exponential backoff. Fan continues operating on last known state during WiFi outage (buttons still work).

#### 6. HTTP Server + REST API (`components/api/`)
- ESP-IDF `esp_http_server` (lightweight, built-in)
- Endpoints:
  - `GET /api/v1/status` — returns current fan state as JSON
  - `POST /api/v1/speed` — body: `{"speed": 75}` (1-100)
  - `POST /api/v1/direction` — body: `{"direction": "intake"|"exhaust"}`
  - `POST /api/v1/mode` — body: `{"mode": "manual"}` (future: "auto")
  - `POST /api/v1/toggle` — toggle on/off
  - `POST /api/v1/stop` — emergency stop
  - `GET /api/v1/events` — SSE stream (long-lived, chunked transfer encoding)
  - `POST /api/v1/ota/update` — OTA firmware upload (Phase 5)
  - `GET /api/v1/info` — firmware version, uptime, free heap, chip info
- All POST endpoints parse JSON body, validate, post command to fan state machine queue, return updated state
- Content-Type: `application/json` for REST, `text/event-stream` for SSE
- No authentication (local network only, Pi is sole client). Add API key header if needed later.

#### 7. OTA Update (`components/ota/`)
- Uses ESP-IDF native OTA: `esp_ota_ops.h`, `esp_https_ota.h` (or raw `esp_ota_begin`/`esp_ota_write`/`esp_ota_end`)
- `POST /api/v1/ota/update` receives firmware binary as streaming body
- Writes to inactive OTA partition
- Validates image header before marking boot
- Calls `esp_ota_set_boot_partition()` then `esp_restart()`
- On first boot after OTA: app must call `esp_ota_mark_app_valid_cancel_rollback()` after confirming stable operation (e.g., WiFi connected, fan driver initialized). If this isn't called within N boots, bootloader rolls back to previous partition.
- OTA endpoint should reject uploads while fan is running (safety) or at minimum stop the fan first.

#### 8. NVS Persistent Settings (`components/settings/`)
- Stored in NVS partition:
  - `wifi_ssid`, `wifi_pass` — WiFi credentials
  - `fan_last_speed` — restore on boot
  - `fan_last_direction` — restore on boot
  - `fan_boot_state` — whether to auto-start fan on power-up (default: off)
  - `hostname` — mDNS hostname (default: "vanfan")
- Read at boot, written on state change (debounce writes, don't thrash flash — write at most once per 5 seconds on rapid changes)

## Build Phases (ordered by dependency)

### Phase 1: Project Scaffold
- `idf.py create-project vanfan`
- Set target: `idf.py set-target esp32s3`
- Custom partition table CSV (see above)
- Configure sdkconfig: flash size 4MB, PSRAM enabled, partition table custom
- CMake component structure: `components/bts7960/`, `components/buttons/`, `components/fan_control/`, `components/event_emitter/`, `components/api/`, `components/ota/`, `components/settings/`
- Stub `app_main()` that boots, prints chip info, confirms partition layout

### Phase 2: BTS7960 Driver + Buttons
- Implement `bts7960` component with LEDC PWM, enable GPIO, soft ramp
- Implement `buttons` component with debounce
- Test: buttons directly control fan via hardcoded command mapping (no state machine yet, just button → driver call)
- Validate PWM frequency, direction switching, soft start/stop on real hardware

### Phase 3: Fan State Machine
- Implement `fan_control` component with FreeRTOS task + command queue
- Wire buttons to post commands to queue
- Wire state machine to call BTS7960 driver
- Implement event notification callback (wire to event emitter in Phase 4)
- Test: buttons control fan through state machine, state is consistent

### Phase 4: WiFi + REST API + SSE
- Implement WiFi STA connection with NVS credential storage
- Implement mDNS registration
- Implement HTTP server with all REST endpoints
- Implement SSE event stream, wire to event emitter
- Test: Pi can GET status, POST commands, and receive SSE events. Button presses show up on SSE stream.

### Phase 5: OTA
- Implement OTA endpoint
- Implement rollback validation logic
- Test: upload new firmware via curl from Pi, verify swap and rollback

### Phase 6: Hardening
- NVS settings persistence with write debounce
- Brownout detection handler (save state, safe-stop fan)
- BTS7960 current sense via ADC for stall/overload detection
- Watchdog timer integration
- Boot state restoration from NVS

## Pin Assignment Reference (preliminary — finalize at wiring)
```
BTS7960 RPWM    → GPIO TBD (needs LEDC-capable pin)
BTS7960 LPWM    → GPIO TBD (needs LEDC-capable pin)
BTS7960 R_EN    → GPIO TBD
BTS7960 L_EN    → GPIO TBD
BTS7960 R_IS    → GPIO TBD (ADC-capable: 10, 14, 15, 16, 17, or 18)
BTS7960 L_IS    → GPIO TBD (ADC-capable: 10, 14, 15, 16, 17, or 18)
Button: On/Off  → GPIO TBD (with pull-up)
Button: Dir     → GPIO TBD (with pull-up)
Button: Speed   → GPIO TBD (with pull-up)
Status LED      → IO0 (onboard green LED)
RGB LED         → IO46 (onboard WS2812, optional status indicator)
```

## Constraints and Notes
- 4MB flash is tight. Keep binary lean. No SPIFFS, no embedded web UI, no large string tables.
- Single client (Pi) is the design target for SSE, but support up to 4 connections for debugging (e.g., curl from laptop while Pi is connected).
- Fan must continue operating during WiFi outage. Buttons always work. Network is secondary control path.
- 12V van power is noisy. Brownout detection is important. ESP32-S3 brownout threshold should be configured appropriately.
- Temperature sensor integration is a future feature. Stub the interface but do not implement auto mode.
- No authentication on API for now. Local network trust model. Can add API key header later if needed.
