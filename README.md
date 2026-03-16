# VanFan Controller

ESP32-S3 roof fan controller for a Sprinter camper van. Controls a DC motor via a BTS7960 dual H-bridge driver with soft-start ramping, exposes a REST API with Server-Sent Events for integration with a Raspberry Pi dashboard, supports physical 2-button control, BLE WiFi provisioning, and OTA firmware updates.

Built on **ESP-IDF v5.2.3** with a Docker-based build pipeline.

## Features

- **Motor control** — BTS7960 H-bridge with 25kHz PWM, asymmetric soft-start ramping, direction flip with coast pause
- **2-button interface** — Speed cycling + direction toggle, hold gestures for on/off
- **REST API** — JSON endpoints for speed, direction, toggle, emergency stop
- **Server-Sent Events** — Real-time state streaming to connected clients
- **OTA updates** — Streaming firmware upload via HTTP
- **BLE WiFi provisioning** — Chrome Web Bluetooth UI for field credential updates
- **mDNS** — Discoverable as `vanfan.local`
- **LED lighting** — 3-zone PWM dimming via IRLZ34N MOSFETs, independent brightness control, dedicated button
- **Status LED** — WS2812B RGB feedback for WiFi/BLE state

## Hardware

| Component | Details |
|-----------|---------|
| MCU | ESP32-S3 (QFN56), dual core 240MHz, 4MB flash, 2MB PSRAM |
| Motor driver | BTS7960 dual H-bridge (5V VCC, 3.3V logic compatible) |
| Buttons | 3x momentary (Speed + Direction + Light), internal pull-up, active low |
| LED lighting | 3x IRLZ34N MOSFETs, PWM dimming via LEDC |
| Status LED | WS2812B RGB on GPIO46 |
| Console | USB-Serial/JTAG (GPIO19/20) |

### Pinout

| Signal | GPIO | Direction | Description |
|--------|------|-----------|-------------|
| RPWM | 5 | Output | Forward (exhaust) PWM — LEDC CH0, 25kHz |
| LPWM | 6 | Output | Reverse (intake) PWM — LEDC CH1, 25kHz |
| R_EN | 7 | Output | Right enable (HIGH on init) |
| L_EN | 15 | Output | Left enable (HIGH on init) |
| R_IS | 4 | Input | Right current sense (ADC, future) |
| L_IS | 3 | Input | Left current sense (ADC, future) |
| Speed button | 9 | Input | Internal pull-up, active low |
| Direction button | 10 | Input | Internal pull-up, active low |
| LED Zone 1 | 11 | Output | LEDC CH2, 1kHz PWM |
| LED Zone 2 | 12 | Output | LEDC CH3, 1kHz PWM |
| LED Zone 3 | 13 | Output | LEDC CH4, 1kHz PWM |
| Light button | 14 | Input | Internal pull-up, active low |
| Status LED | 46 | Output | WS2812B via RMT peripheral |

All pins are configurable via Kconfig (`VanFan Controller` menu).

## Getting Started

### Prerequisites

- Docker (for building)
- Python 3 with `esptool` (for flashing)
- ESP32-S3 connected via USB

### Build

WiFi credentials are injected at build time:

```bash
# Option 1: Create .env file
echo 'WIFI_SSID=MyNetwork' > .env
echo 'WIFI_PASSWORD=MyPassword' >> .env
./build.sh

# Option 2: Pass as arguments
./build.sh MyNetwork MyPassword
```

This runs the build inside Docker (`espressif/idf:v5.2.3`) and extracts binaries to `firmware/`.

### Flash

```bash
./flash.sh
```

Flashes all partitions including bootloader, partition table, OTA data, and application binary. The ESP32-S3 must be connected via USB (appears as `/dev/cu.usbmodem*`).

If the device is in a crash loop, hold **BOOT** + press **RST** to enter bootloader mode.

### Monitor

```bash
./monitor.sh
```

Opens a serial monitor at 115200 baud. Note: boot messages are lost after reset because USB disconnects during the reset cycle. The firmware includes a 3-second startup delay to allow reconnection.

## Architecture

```
main.c (boot sequence + heartbeat)
├── bts7960        — Motor driver (LEDC PWM + GPIO)
├── fan_control    — State machine + command queue
├── buttons        — Button polling + debounce (speed + direction)
├── led_control    — 3-zone LED PWM dimming
├── light_button   — Light button polling + debounce
├── event_emitter  — SSE client management (fan + lights)
├── wifi           — WiFi manager + mDNS + auto-reconnect
├── api            — REST API endpoints (HTTP server)
├── ota            — OTA firmware update handler
├── ble_prov       — BLE WiFi provisioning
├── status_led     — WS2812B RGB status indicator
└── settings       — NVS persistence (stub)
```

### Boot Sequence

1. 3-second USB delay
2. NVS flash init
3. Status LED init
4. BTS7960 motor driver init
5. Fan control state machine init
6. Button polling task start (speed + direction)
7. LED control init (3-zone PWM)
8. Light button polling task start
9. Event emitter (SSE) init
10. WiFi manager init (connects in background)
11. BLE provisioning init
12. HTTP API server start
13. OTA boot validation
14. Heartbeat loop (5-second state logging)

### Key Design Patterns

**Command queue** — Button presses and API calls post commands to a FreeRTOS queue. The fan control task processes them serially, ensuring atomic state transitions.

**Callback chain** — Fan state changes propagate via registered callbacks to the event emitter (SSE) and other subscribers. Up to 4 callbacks supported.

**Non-blocking WiFi** — WiFi connects in the background with exponential backoff reconnect (1s to 60s max). Fan and buttons work independently of WiFi.

**Soft-start ramping** — One-shot `esp_timer` re-scheduled each PWM step. Ramp up: 20ms/step (2s full range), ramp down: 50ms/step (5s full range). Direction flip: ramp to 0 → 2s coast (enable pins LOW) → ramp up.

### FreeRTOS Tasks

| Task | Stack | Priority | Purpose |
|------|-------|----------|---------|
| `fan_control` | 4096 | 10 | Command queue + state machine |
| `btn_poll` | 4096 | 5 | Fan button polling + debounce |
| `light_btn` | 4096 | 5 | Light button polling + debounce |
| `status_led` | 4096 | 2 | LED blink state machine |
| `app_main` | 8192 | 1 | Boot + heartbeat |

## Button Control

Two-button interface with press and hold gestures:

| State | Speed Button | Direction Button |
|-------|-------------|-----------------|
| **Fan OFF** + press | Turn on (resume last config) | Turn on (resume last config) |
| **Fan OFF** + hold | Turn on at 20% intake | Turn on at 20% exhaust |
| **Fan ON** + press | Cycle speed: 20→40→60→80→100→20 | Toggle direction |
| **Fan ON** + hold | Turn off | Turn off |
| **Any** + hold both | Enter/exit BLE provisioning mode | — |

- Hold threshold: 800ms
- Debounce: 50ms
- 10ms polling interval

### Light Button

Dedicated button for LED lighting control:

| Action | Result |
|--------|--------|
| Press | Toggle zone (default: zone 1, configurable via API) |
| Hold | All zones off |

## REST API

**Base URL:** `http://vanfan.local/api/v1` (mDNS) or `http://<device-ip>/api/v1`

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/status` | Current fan state |
| `POST` | `/speed` | Set speed (1-100), turns fan on |
| `POST` | `/direction` | Set `"exhaust"` or `"intake"` |
| `POST` | `/mode` | Set mode (`"manual"` only) |
| `POST` | `/set` | Combined speed + direction (both optional) |
| `POST` | `/toggle` | Toggle on/off |
| `POST` | `/stop` | Emergency brake (no ramp) |
| `GET` | `/events` | SSE stream (fan + lights) |
| `POST` | `/ota/update` | Upload firmware binary |
| `GET` | `/info` | Version, uptime, heap, chip info |
| `GET` | `/wifi` | WiFi status and credential source |
| `POST` | `/wifi/reset` | Clear NVS creds, use build-time |
| `GET` | `/lights` | Current light zone states |
| `POST` | `/lights/zone` | Set single zone |
| `POST` | `/lights/zones` | Set multiple zones |
| `POST` | `/lights/all` | Set all zones uniformly |
| `POST` | `/lights/off` | All zones off |
| `GET` | `/lights/button` | Light button press action |
| `POST` | `/lights/button` | Configure light button press action |

### Examples

```bash
# Get current state
curl http://vanfan.local/api/v1/status

# Set speed to 75%
curl -X POST -H 'Content-Type: application/json' \
     -d '{"speed": 75}' http://vanfan.local/api/v1/speed

# Set speed and direction atomically
curl -X POST -H 'Content-Type: application/json' \
     -d '{"speed": 50, "direction": "exhaust"}' http://vanfan.local/api/v1/set

# Toggle on/off
curl -X POST http://vanfan.local/api/v1/toggle

# Emergency stop
curl -X POST http://vanfan.local/api/v1/stop

# Subscribe to SSE events
curl -N http://vanfan.local/api/v1/events

# OTA update
curl -X POST --data-binary @firmware/vanfan.bin \
     http://vanfan.local/api/v1/ota/update

# Device info
curl http://vanfan.local/api/v1/info

# Get light states
curl http://vanfan.local/api/v1/lights

# Set zone 1 to 100% brightness
curl -X POST -H 'Content-Type: application/json' \
     -d '{"zone":1,"on":true,"brightness":100}' http://vanfan.local/api/v1/lights/zone

# Set multiple zones
curl -X POST -H 'Content-Type: application/json' \
     -d '{"zones":[{"zone":1,"brightness":80},{"zone":3,"on":true,"brightness":30}]}' \
     http://vanfan.local/api/v1/lights/zones

# All zones to 50%
curl -X POST -H 'Content-Type: application/json' \
     -d '{"on":true,"brightness":50}' http://vanfan.local/api/v1/lights/all

# All zones off
curl -X POST http://vanfan.local/api/v1/lights/off

# WiFi status
curl http://vanfan.local/api/v1/wifi

# Clear NVS WiFi credentials
curl -X POST http://vanfan.local/api/v1/wifi/reset

# Get light button config
curl http://vanfan.local/api/v1/lights/button

# Set light button to toggle all zones
curl -X POST -H 'Content-Type: application/json' \
     -d '{"action":"all"}' http://vanfan.local/api/v1/lights/button

# Set light button to toggle zone 2
curl -X POST -H 'Content-Type: application/json' \
     -d '{"action":"zone","zone":2}' http://vanfan.local/api/v1/lights/button
```

### State Response Format

```json
{
  "running": true,
  "speed": 50,
  "direction": "exhaust",
  "mode": "manual"
}
```

### SSE Event Format

Fan events (unnamed):
```
data: {"running":true,"speed":50,"direction":"exhaust","mode":"manual","source":"button"}
```

Light events (named `lights`):
```
event: lights
data: {"zones":[{"on":true,"brightness":50},{"on":false,"brightness":50},{"on":false,"brightness":50}],"source":"button"}
```

SSE behavior:
- Sends current fan and light state immediately on connection
- Streams updates on every fan or light state change
- Fan events: use `onmessage`; light events: use `addEventListener("lights", ...)`
- `source` field: `"button"`, `"api"`, or `"startup"`
- 15-second keepalive comments (`: keepalive`)
- Maximum 4 concurrent SSE clients

### Error Responses

```json
{
  "error": 400,
  "message": "Invalid JSON"
}
```

| Code | Meaning |
|------|---------|
| 400 | Bad request (malformed JSON, missing field) |
| 422 | Validation error (out-of-range value, invalid enum) |
| 500 | Internal error |

### Pi Integration

Recommended patterns for the Raspberry Pi dashboard:

1. **Use `/set` for control** — handles speed + direction atomically, auto-starts
2. **Use SSE for state sync** — subscribe to `/events` instead of polling
3. **Handle disconnects** — reconnect SSE with backoff; first event contains full state
4. **Speed values** — buttons cycle 20/40/60/80/100, but API accepts any value 1-100
5. **Direction changes are slow** — ramp down + 2s coast + ramp up; API responds immediately with target state

## OTA Updates

Upload a firmware binary over HTTP:

```bash
curl -X POST --data-binary @firmware/vanfan.bin \
     http://vanfan.local/api/v1/ota/update
```

The device will:
1. Emergency-stop the fan (safety)
2. Stream the binary to the alternate OTA partition in 1KB chunks
3. Validate the firmware image
4. Set the new partition as boot target
5. Restart

### Partition Table

```
nvs        0x9000    24KB    NVS storage
otadata    0xf000     8KB    OTA boot metadata
ota_0      0x20000  1.5MB    Application slot A
ota_1      0x1A0000 1.5MB    Application slot B
```

Current binary size: **1159KB** (well under 1.5MB limit).

## BLE WiFi Provisioning

For updating WiFi credentials in the field without reflashing.

### Entering Provisioning Mode

Hold both Speed and Direction buttons for 800ms. The status LED turns solid blue.

During provisioning:
- WiFi is suspended (driver stays running for credential verification) and HTTP server is stopped
- BLE advertises as `VANFAN`
- Fan continues running; single-button presses still work
- A 5-second grace period prevents accidental exit

### Using the Web Bluetooth UI

1. Open `tools/provision.html` in Chrome
2. Click **Scan for VanFan**
3. Select the `VANFAN` device
4. Enter new WiFi SSID and password
5. Click **Send Credentials**

Credentials are stored in NVS. The device restarts WiFi with the new credentials automatically.

### Exiting Provisioning Mode

- **After credentials received:** Automatic — WiFi restarts, API server comes back up, LED flashes green then turns off
- **Manual cancel:** Hold both buttons again (after 5-second grace period)

### Status LED States

| State | LED |
|-------|-----|
| WiFi connected | Off |
| WiFi disconnected | Red slow blink (~1Hz) |
| BLE provisioning | Blue solid |
| Credentials received | Green flash (2s) |
| Credentials failed | Rapid red blink (3s), then back to blue |
| WiFi credential reset | Rapid yellow blink (3s) |

## WiFi Resilience

The controller supports multiple WiFi credential sources with automatic fallback:

### Credential Priority

1. **NVS credentials** (from BLE provisioning) — highest priority
2. **Build-time credentials** (from `.env` file) — fallback

On boot and reconnect, the device scans for visible networks and connects to the best known match. Both credential sets remain available — if the NVS network disappears, it automatically falls back to the build-time network.

### Clearing Stale NVS Credentials

NVS credentials persist across reflashes. If BLE-provisioned credentials become stale:

**Boot-time reset:** Hold the **light button** during power-on. The status LED blinks yellow for 3 seconds, confirming the reset. The device then connects using build-time credentials.

**API reset:** Send `POST /api/v1/wifi/reset` to clear NVS credentials and reconnect with build-time credentials.

### Fan Independence

The fan continues running at its current speed regardless of WiFi state. Buttons always work. WiFi reconnects automatically in the background with exponential backoff (1s to 60s).

## Project Structure

```
FanController/
├── main/
│   ├── main.c                 # Entry point + boot sequence
│   ├── Kconfig.projbuild      # Pin and WiFi configuration
│   └── idf_component.yml      # Managed dependencies (mdns, led_strip)
├── components/
│   ├── bts7960/               # Motor driver (PWM + ramp timer)
│   ├── buttons/               # Button polling + debounce
│   ├── fan_control/           # State machine + command queue
│   ├── event_emitter/         # SSE client management
│   ├── wifi/                  # WiFi manager + mDNS + reconnect
│   ├── api/                   # REST API endpoints
│   ├── ota/                   # OTA update handler
│   ├── led_control/           # 3-zone LED PWM dimming
│   ├── light_button/          # Light button polling
│   ├── ble_prov/              # BLE WiFi provisioning
│   ├── status_led/            # WS2812B RGB LED
│   └── settings/              # NVS persistence (stub)
├── tools/
│   └── provision.html         # Web Bluetooth provisioning UI
├── docs/
│   └── api.md                 # Detailed API reference
├── firmware/                  # Build output (binaries)
├── build.sh                   # Docker build script
├── flash.sh                   # Flash via USB
├── monitor.sh                 # Serial monitor
├── Dockerfile                 # Build environment
├── partitions.csv             # OTA partition table
├── sdkconfig.defaults         # ESP-IDF configuration
├── version.txt                # Firmware version (0.4.0)
└── pinout.md                  # Hardware pinout reference
```

## Configuration

All runtime configuration is set via Kconfig (`main/Kconfig.projbuild`) under the `VanFan Controller` menu:

| Setting | Kconfig Key | Default |
|---------|-------------|---------|
| WiFi SSID | `VANFAN_WIFI_SSID` | (injected at build) |
| WiFi Password | `VANFAN_WIFI_PASSWORD` | (injected at build) |
| mDNS Hostname | `VANFAN_MDNS_HOSTNAME` | `vanfan` |
| RPWM Pin | `VANFAN_PIN_RPWM` | 5 |
| LPWM Pin | `VANFAN_PIN_LPWM` | 6 |
| R_EN Pin | `VANFAN_PIN_R_EN` | 7 |
| L_EN Pin | `VANFAN_PIN_L_EN` | 15 |
| R_IS Pin | `VANFAN_PIN_R_IS` | 4 |
| L_IS Pin | `VANFAN_PIN_L_IS` | 3 |
| Speed Button | `VANFAN_PIN_BTN_SPEED` | 9 |
| Direction Button | `VANFAN_PIN_BTN_DIRECTION` | 10 |
| Status LED | `VANFAN_PIN_STATUS_LED` | 46 |
| LED Zone 1 | `VANFAN_PIN_LED_ZONE1` | 11 |
| LED Zone 2 | `VANFAN_PIN_LED_ZONE2` | 12 |
| LED Zone 3 | `VANFAN_PIN_LED_ZONE3` | 13 |
| Light Button | `VANFAN_PIN_BTN_LIGHT` | 14 |

Key `sdkconfig.defaults` settings:

```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
```

## Dependencies

**ESP-IDF managed components** (in `main/idf_component.yml`):
- `espressif/mdns ^1.2` — mDNS service advertisement
- `espressif/led_strip ^2.5` — WS2812B LED driver

**ESP-IDF built-in components:**
- `esp_wifi`, `esp_netif`, `esp_event` — networking
- `esp_http_server` — HTTP/REST
- `driver` — GPIO, LEDC PWM
- `esp_timer` — one-shot ramp timers
- `esp_ota_ops`, `esp_partition`, `app_update` — OTA
- `nvs_flash` — non-volatile storage
- `wifi_provisioning` — BLE credential flow
- `bt` — NimBLE Bluetooth stack
- `json` — cJSON library

## Motor Control Details

### Ramp Timing

| Operation | Rate | Full Range |
|-----------|------|------------|
| Ramp up | 20ms per 1% | ~2 seconds |
| Ramp down | 50ms per 1% | ~5 seconds |
| Direction flip | Ramp to 0 → 2s coast → ramp up | ~9 seconds max |

During coast (direction flip), enable pins are driven LOW so the motor spins freely. The one-shot timer is re-scheduled each step to support variable-rate ramping.

### Speed Cycling

Button press cycles through: **20 → 40 → 60 → 80 → 100 → 20** (wrap). If current speed is between steps (set via API), the next step above current speed is used (values below 40 jump to 40).

## Troubleshooting

**No serial output after flash** — USB-Serial/JTAG disconnects during reset. Wait 3 seconds for the startup delay, then reconnect the monitor.

**Crash loop on boot** — Hold BOOT + press RST to enter bootloader mode, then reflash.

**WiFi won't connect** — Check credentials in `.env`. If credentials changed, use BLE provisioning (hold both buttons) to update them without reflashing.

**OTA fails** — Ensure binary is under 1.5MB. The device emergency-stops the fan during upload. If OTA validation fails, the device continues running the current firmware.

**Button task crash (spinlock assert)** — Button task stack must be 4096 bytes (already configured). If adding callbacks with logging, verify stack headroom.

## Code Statistics

| Component | Lines |
|-----------|-------|
| api | 765 |
| wifi | 548 |
| fan_control | 354 |
| bts7960 | 302 |
| main | 279 |
| led_control | 274 |
| event_emitter | 257 |
| buttons | 206 |
| ota | 174 |
| status_led | 166 |
| ble_prov | 164 |
| light_button | 132 |
| **Total** | **~3,620** |
