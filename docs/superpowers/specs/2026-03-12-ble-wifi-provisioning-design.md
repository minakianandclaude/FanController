# BLE WiFi Provisioning — Design Spec

## Overview

Add BLE-based WiFi credential provisioning to the VanFan controller. Uses ESP-IDF's `wifi_provisioning` component with BLE transport (NimBLE) and security0. Triggered manually by holding both physical buttons. A Chrome Web Bluetooth page serves as the provisioning client.

## Motivation

Three failure scenarios require credential updates without reflashing:
1. Build-time WiFi credentials are wrong
2. WiFi network changes (new SSID/password)
3. Recovery from misconfiguration

## Design Decisions

- **Manual trigger only** — hold both buttons (800ms) to enter BLE provisioning mode. No automatic fallback.
- **Exclusive mode** — WiFi/HTTP/mDNS shut down while BLE is active. Shared radio, simpler state management.
- **Fan stays operational** — motor and button controls (speed/direction) continue working during provisioning.
- **Security 0** — no encryption on BLE transport. Acceptable for single-user personal device. Can upgrade to security1 later.
- **ESP-IDF `wifi_provisioning`** — battle-tested framework, handles NVS credential storage automatically.
- **NimBLE stack** — lighter than Bluedroid, sufficient for provisioning use case.
- **Web Bluetooth client** — Chrome-based provisioning page instead of `esp_prov.py`. Standalone HTML file (not served from ESP32 since WiFi is down during provisioning).
- **No provisioning timeout** — device stays in BLE mode indefinitely until credentials are received or user cancels manually. Intentional: single-user device, physical access required to exit.

## Boot Flow & Credential Priority

1. Boot → NVS init (existing)
2. Fan/buttons/motor init independently (unchanged, before WiFi)
3. Inside `wifi_manager_init()`, the credential check sequence is:
   a. `esp_wifi_init()` — initialize WiFi driver
   b. `wifi_prov_mgr_init()` — initialize provisioning manager (requires WiFi driver)
   c. `wifi_prov_mgr_is_provisioned()` — check NVS for BLE-provisioned credentials
   d. If NOT provisioned → `esp_wifi_set_config()` with Kconfig defaults
   e. If provisioned → skip `esp_wifi_set_config()` (WiFi driver reads NVS creds automatically with `WIFI_STORAGE_FLASH`, the default)
   f. `wifi_prov_mgr_deinit()` — release provisioning manager resources (not needed until BLE trigger)
   g. `esp_wifi_start()` — connect

**Key detail:** `wifi_prov_mgr_is_provisioned()` requires the provisioning manager to be initialized first, so `wifi_prov_mgr_init()` / `wifi_prov_mgr_deinit()` bracket the check. This adds `wifi_provisioning` as a dependency of the `wifi` component.

Exponential backoff reconnection continues as-is. No automatic BLE fallback.

## BLE Provisioning Mode

### Entry: Hold Both Buttons (800ms)

Detected as a distinct gesture from single-button holds. When both buttons are simultaneously in the `PRESSED` state and reach the 800ms hold threshold, fire `BTN_EVT_HOLD_BOTH` **instead of** two individual `BTN_EVT_HOLD` events. Both buttons transition directly to `WAIT_RELEASE` — the individual hold callbacks are suppressed.

Sequence on entry:
1. Cancel any pending WiFi reconnect timer (`xTimerStop`)
2. Stop mDNS (`mdns_free()`)
3. Stop HTTP server (`api_stop()`) — cleans up active SSE connections
4. Disconnect and stop WiFi (`esp_wifi_disconnect()` + `esp_wifi_stop()` + `esp_wifi_deinit()`)
5. Start BLE provisioning (advertise as "VANFAN")
6. Set status LED to solid blue

**Important:** `wifi_manager_stop()` must NOT deinit `esp_netif` or the default event loop — these are global singletons initialized once. Only the WiFi driver is stopped/deinitialized. The reconnect timer must be cancelled before disconnect to prevent the timer callback firing into a deinitialized WiFi driver.

### While in Provisioning Mode

- Fan continues running if it was on
- Single-button presses still control fan (speed cycle / direction toggle)
- BLE advertising, waiting for client connection
- 5-second grace period after entry before hold-both can trigger cancel (prevents accidental exit from button release)

### Exit Path 1: Credentials Received

1. `wifi_provisioning` stores new SSID/password in NVS
2. Stop BLE
3. LED flashes green (2 seconds)
4. Start WiFi with new credentials (`esp_wifi_init()` + `esp_wifi_start()` — driver reads NVS creds)
5. mDNS restarts automatically via existing `IP_EVENT_STA_GOT_IP` handler
6. Start HTTP server (`api_start()`)
7. LED off (normal operation)

### Exit Path 2: Manual Cancel

After 5-second grace period, hold both buttons again to cancel (symmetric gesture — same action to enter and exit):
1. Stop BLE
2. Start WiFi with previous credentials (NVS or compile-time)
3. mDNS restarts automatically via `IP_EVENT_STA_GOT_IP` handler
4. Start HTTP server (`api_start()`)
5. LED off

## Status LED

WS2812B addressable RGB LED on GPIO 46. Driven via `espressif/led_strip` managed component (RMT peripheral).

| State | LED Behavior |
|-------|-------------|
| Normal operation (WiFi connected) | Off |
| WiFi disconnected (retrying) | Red slow blink (~1Hz) |
| BLE provisioning active | Blue solid |
| Credentials received | Green flash (2s), then off |

LED controlled by a FreeRTOS task (4096 stack, consistent with other tasks in this project) that runs a state machine for blink patterns.

## Component Architecture

### New Components

#### `status_led`
- API: `status_led_init()`, `status_led_set_state(status_led_state_t)`
- States: `STATUS_LED_OFF`, `STATUS_LED_WIFI_DISCONNECTED`, `STATUS_LED_BLE_PROVISIONING`, `STATUS_LED_SUCCESS`
- FreeRTOS task for blink patterns, 4096 stack
- Depends on: `espressif/led_strip`

#### `ble_prov`
- API: `ble_prov_init()`, `ble_prov_start()`, `ble_prov_stop()`, `ble_prov_is_active()`
- Wraps `wifi_provisioning` manager with BLE transport
- Security 0, service name "VANFAN"
- Callbacks: on credentials received (to trigger WiFi restart)
- Depends on: `wifi_provisioning`, `protocomm`, `bt` (NimBLE)

### Modified Components

#### `wifi` (wifi_manager)
- Add `wifi_provisioning` to CMakeLists.txt REQUIRES (needed for `wifi_prov_mgr_is_provisioned()` check at boot)
- Add `wifi_manager_stop()` — cancel reconnect timer, then `esp_wifi_disconnect()` + `esp_wifi_stop()` + `esp_wifi_deinit()`. Must NOT deinit `esp_netif` or the default event loop (global singletons created once).
- Add `wifi_manager_start()` — `esp_wifi_init()` + `esp_wifi_set_mode(STA)` + `esp_wifi_start()`. Re-register event handlers if needed. WiFi driver reads NVS creds automatically.
- Modify `wifi_manager_init()` — check `wifi_prov_mgr_is_provisioned()`. Only call `esp_wifi_set_config()` with Kconfig values if NOT provisioned.
- Add `wifi_manager_is_connected()` state change callback for LED updates

#### `event_emitter`
- Add `event_emitter_stop()` — stop keepalive timer, mark all SSE clients inactive, clear client list. Must be called BEFORE `httpd_stop()` to prevent stale `httpd_handle_t` usage (keepalive timer and fan state change callbacks would crash with freed handle).
- Add `event_emitter_start()` — restart keepalive timer (client list starts empty; new SSE connections register after HTTP server restarts).

#### `api`
- Add `api_stop()` — call `event_emitter_stop()` first, then `httpd_stop(s_server)`
- Add `api_start()` — restart HTTP server with same configuration, then call `event_emitter_start()`
- Server handle `s_server` already exists as static; stop/start toggle its lifecycle

#### `buttons`
- Add "hold both" gesture detection: when both buttons are simultaneously in `PRESSED` state and reach 800ms threshold, fire `BTN_EVT_HOLD_BOTH` and suppress individual `BTN_EVT_HOLD` events. Both buttons transition to `WAIT_RELEASE`.
- Add `BTN_ID_BOTH` to `button_id_t` enum — passed as the `btn` parameter when `BTN_EVT_HOLD_BOTH` fires (the event type is sufficient to identify the gesture; `BTN_ID_BOTH` makes the API unambiguous).
- During provisioning mode, single presses still control fan; hold-both toggles provisioning mode

#### `fan_control`
- Remove direct `buttons_register_callback()` call from `fan_control_init()`.
- Add `fan_control_button_event(button_id_t btn, button_event_t evt)` — public function that `main.c` calls to forward button events. Same logic as current `button_callback`, just no longer registered directly.

#### `main.c`
- **Unified button dispatcher:** `main.c` registers a single callback with `buttons_register_callback()`. Routes events:
  - `BTN_EVT_HOLD_BOTH` → provisioning enter/exit logic
  - All other events → `fan_control_button_event()`
- Init `status_led` early in boot sequence
- Wire up provisioning events (credentials received) to WiFi restart + HTTP server restart
- Set LED state on WiFi connect/disconnect events

### New Managed Components

In `main/idf_component.yml`:
- `espressif/led_strip: "^2.5"` — WS2812B RMT driver

### sdkconfig.defaults Additions

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

Note: `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y` is needed because PSRAM is currently disabled in sdkconfig.

## Web Bluetooth Provisioning Page

### File: `tools/provision.html`

Standalone HTML file opened locally in Chrome. Not served from ESP32.

### Functionality

1. "Connect" button → `navigator.bluetooth.requestDevice()` filtering for "VANFAN"
2. Connect to GATT server → discover provisioning service/characteristics
3. Form: SSID input + password input + "Provision" button
4. Encode credentials using protocomm security0 protobuf format
5. Write to appropriate GATT characteristics
6. Display success/failure status

### protocomm Details

- Uses custom GATT service UUID defined by `wifi_provisioning`
- Characteristics for session establishment and credential exchange
- Security 0: no encryption handshake, direct protobuf write
- Protobuf schemas from ESP-IDF: `wifi_config.proto`, `session.proto`
- JS protobuf encoding via lightweight library (e.g., protobuf.js) or manual encoding

### Browser Requirements

- Chrome 56+ (Web Bluetooth API)
- macOS, Linux, or Android (Web Bluetooth not supported on Firefox/Safari)

## Binary Size Impact

- Current: 877KB
- BLE (NimBLE) stack: ~150-200KB
- `wifi_provisioning` + `protocomm`: ~50-80KB
- `led_strip`: ~10KB
- Estimated total: ~1.1-1.2MB
- Partition limit: 1.5MB — sufficient headroom

## Testing

1. Build and flash firmware with BLE enabled
2. Verify normal boot: WiFi connects with compile-time creds, LED off
3. Hold both buttons → LED turns blue, WiFi stops
4. Open `tools/provision.html` in Chrome
5. Connect to "VANFAN", enter new SSID/password, submit
6. Verify: LED green flash, WiFi connects with new creds, API accessible
7. Reboot → verify NVS creds persist (connects without compile-time defaults)
8. Hold both buttons again (after 5s) → hold both again to cancel → verify WiFi reconnects with previous creds
9. Verify fan control (single-button press/hold) works throughout all states
10. Verify hold-both does NOT trigger two individual hold events (fan should not turn off when entering provisioning)
