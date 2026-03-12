# BLE WiFi Provisioning Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add BLE-based WiFi credential provisioning triggered by holding both buttons, with WS2812B status LED and Chrome Web Bluetooth client.

**Architecture:** ESP-IDF `wifi_provisioning` component with NimBLE transport and security0. Manual trigger via dual-button hold. Exclusive mode — WiFi/HTTP shut down during BLE. Status LED indicates device state via WS2812B on GPIO 46.

**Tech Stack:** ESP-IDF v5.2.3, NimBLE, wifi_provisioning/protocomm, espressif/led_strip (RMT), Web Bluetooth API

**Spec:** `docs/superpowers/specs/2026-03-12-ble-wifi-provisioning-design.md`

---

## File Structure

### New Files
| File | Responsibility |
|------|---------------|
| `components/status_led/status_led.c` | WS2812B LED driver + blink pattern task |
| `components/status_led/include/status_led.h` | Public API: init, set_state |
| `components/status_led/CMakeLists.txt` | Component registration |
| `components/ble_prov/ble_prov.c` | wifi_provisioning wrapper, BLE start/stop |
| `components/ble_prov/include/ble_prov.h` | Public API: init, start, stop, is_active |
| `components/ble_prov/CMakeLists.txt` | Component registration |
| `tools/provision.html` | Chrome Web Bluetooth provisioning page |

### Modified Files
| File | Change |
|------|--------|
| `components/buttons/include/buttons.h` | Add `BTN_ID_BOTH`, `BTN_EVT_HOLD_BOTH` |
| `components/buttons/buttons.c` | Hold-both gesture detection |
| `components/fan_control/include/fan_control.h` | Add `fan_control_button_event()` |
| `components/fan_control/fan_control.c` | Extract button_callback to public function, remove register_callback call |
| `components/wifi/include/wifi_manager.h` | Add stop/start declarations |
| `components/wifi/wifi_manager.c` | NVS credential check, stop/start, suppress reconnect flag |
| `components/wifi/CMakeLists.txt` | Add wifi_provisioning dependency |
| `components/event_emitter/include/event_emitter.h` | Add stop/start declarations |
| `components/event_emitter/event_emitter.c` | Stop/start lifecycle |
| `components/api/include/api.h` | Add stop/start declarations |
| `components/api/api.c` | Stop/start lifecycle |
| `main/main.c` | Unified button dispatcher, status LED init, provisioning orchestration |
| `main/CMakeLists.txt` | Add status_led, ble_prov dependencies |
| `main/idf_component.yml` | Add espressif/led_strip |
| `main/Kconfig.projbuild` | Add LED GPIO config |
| `sdkconfig.defaults` | Add BT/NimBLE config |

---

## Chunk 1: Status LED Component

### Task 1: Create status_led component scaffold

**Files:**
- Create: `components/status_led/include/status_led.h`
- Create: `components/status_led/status_led.c`
- Create: `components/status_led/CMakeLists.txt`
- Modify: `main/idf_component.yml`
- Modify: `main/Kconfig.projbuild`

- [ ] **Step 1: Add LED GPIO to Kconfig**

Add to `main/Kconfig.projbuild` before the closing `endmenu`:

```c
    menu "Status LED"
        config VANFAN_PIN_STATUS_LED
            int "Status LED GPIO (WS2812B)"
            default 46
    endmenu
```

- [ ] **Step 2: Add led_strip managed component**

Add to `main/idf_component.yml`:

```yaml
dependencies:
  espressif/mdns: "^1.2"
  espressif/led_strip: "^2.5"
```

- [ ] **Step 3: Create status_led header**

Create `components/status_led/include/status_led.h`:

```c
#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_WIFI_DISCONNECTED,   // red slow blink ~1Hz
    STATUS_LED_BLE_PROVISIONING,    // blue solid
    STATUS_LED_SUCCESS,             // green 2s then off
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set_state(status_led_state_t state);
```

- [ ] **Step 4: Create status_led implementation**

Create `components/status_led/status_led.c`:

```c
#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#define TASK_STACK_SIZE  4096
#define TASK_PRIORITY    2

static led_strip_handle_t s_strip;
static volatile status_led_state_t s_state = STATUS_LED_OFF;
static TaskHandle_t s_task_handle;

static void set_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void clear_pixel(void)
{
    led_strip_clear(s_strip);
}

static void led_task(void *arg)
{
    status_led_state_t current = STATUS_LED_OFF;
    int tick = 0;
    int success_ticks = 0;

    while (1) {
        status_led_state_t desired = s_state;

        // Reset tick counter on state change
        if (desired != current) {
            current = desired;
            tick = 0;
            success_ticks = 0;
            clear_pixel();
        }

        switch (current) {
        case STATUS_LED_OFF:
            clear_pixel();
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_WIFI_DISCONNECTED:
            // Red blink ~1Hz: 500ms on, 500ms off
            if (tick % 2 == 0) {
                set_pixel(32, 0, 0);  // dim red
            } else {
                clear_pixel();
            }
            tick++;
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_BLE_PROVISIONING:
            set_pixel(0, 0, 32);  // dim blue solid
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_SUCCESS:
            // Green for 2 seconds, then auto-off
            if (success_ticks < 4) {  // 4 x 500ms = 2s
                set_pixel(0, 32, 0);  // dim green
                success_ticks++;
            } else {
                s_state = STATUS_LED_OFF;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_VANFAN_PIN_STATUS_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    clear_pixel();

    BaseType_t xret = xTaskCreate(led_task, "status_led",
                                   TASK_STACK_SIZE, NULL, TASK_PRIORITY, &s_task_handle);
    if (xret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized on GPIO %d", CONFIG_VANFAN_PIN_STATUS_LED);
    return ESP_OK;
}

void status_led_set_state(status_led_state_t state)
{
    s_state = state;
}
```

- [ ] **Step 5: Create CMakeLists.txt**

Create `components/status_led/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "status_led.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip
)
```

- [ ] **Step 6: Wire status_led into main.c**

Add to `main/CMakeLists.txt` REQUIRES: `status_led`

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ota nvs_flash spi_flash bts7960 buttons fan_control event_emitter wifi api status_led
)
```

Add to `main/main.c` after the NVS init block (line 34), before version logging:

```c
#include "status_led.h"

// ... after NVS init, before version log:
    ESP_ERROR_CHECK(status_led_init());
```

- [ ] **Step 7: Build to verify component compiles**

Run: `./build.sh` (or Docker build)
Expected: Clean build, binary size increase ~10KB from led_strip

- [ ] **Step 8: Commit**

```bash
git add components/status_led/ main/idf_component.yml main/Kconfig.projbuild main/CMakeLists.txt main/main.c
git commit -m "feat: add status_led component with WS2812B driver"
```

---

## Chunk 2: Button Hold-Both Gesture + Fan Control Refactor

### Task 2: Add BTN_EVT_HOLD_BOTH and BTN_ID_BOTH to buttons component

**Files:**
- Modify: `components/buttons/include/buttons.h`
- Modify: `components/buttons/buttons.c`

- [ ] **Step 1: Update buttons.h with new enum values**

In `components/buttons/include/buttons.h`, add `BTN_ID_BOTH` and `BTN_EVT_HOLD_BOTH`:

```c
typedef enum {
    BTN_ID_SPEED,
    BTN_ID_DIRECTION,
    BTN_ID_BOTH,
} button_id_t;

typedef enum {
    BTN_EVT_PRESS,
    BTN_EVT_HOLD,
    BTN_EVT_HOLD_BOTH,
} button_event_t;
```

- [ ] **Step 2: Implement hold-both detection in buttons.c**

The key change is in `button_poll_task()`. Before calling `poll_button()` for each button, check if both are in `BTN_STATE_PRESSED` and both have exceeded hold threshold. If so, fire `BTN_EVT_HOLD_BOTH` and transition both to `WAIT_RELEASE`.

Replace `button_poll_task` in `components/buttons/buttons.c`:

```c
static void fire_event(button_id_t btn, button_event_t evt)
{
    ESP_LOGI(TAG, "btn=%s evt=%s",
             btn == BTN_ID_SPEED ? "SPEED" :
             btn == BTN_ID_DIRECTION ? "DIRECTION" : "BOTH",
             evt == BTN_EVT_PRESS ? "PRESS" :
             evt == BTN_EVT_HOLD ? "HOLD" : "HOLD_BOTH");
    if (s_cb) {
        s_cb(btn, evt, s_cb_data);
    }
}

static bool check_hold_both(void)
{
    // Both must be in PRESSED state
    if (s_buttons[0].state != BTN_STATE_PRESSED ||
        s_buttons[1].state != BTN_STATE_PRESSED) {
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed0 = (now - s_buttons[0].state_enter_tick) * portTICK_PERIOD_MS;
    uint32_t elapsed1 = (now - s_buttons[1].state_enter_tick) * portTICK_PERIOD_MS;

    // Both must have been held for HOLD_MS
    if (elapsed0 >= HOLD_MS && elapsed1 >= HOLD_MS) {
        fire_event(BTN_ID_BOTH, BTN_EVT_HOLD_BOTH);
        // Transition both to WAIT_RELEASE (suppresses individual hold events)
        s_buttons[0].state = BTN_STATE_WAIT_RELEASE;
        s_buttons[0].state_enter_tick = now;
        s_buttons[1].state = BTN_STATE_WAIT_RELEASE;
        s_buttons[1].state_enter_tick = now;
        return true;
    }
    return false;
}

static void button_poll_task(void *arg)
{
    while (1) {
        // Check hold-both BEFORE individual polling to suppress individual holds
        if (!check_hold_both()) {
            poll_button(&s_buttons[0]);
            poll_button(&s_buttons[1]);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}
```

- [ ] **Step 3: Build to verify**

Run: `./build.sh`
Expected: Clean build. No behavioral change yet (nothing handles `BTN_EVT_HOLD_BOTH`).

- [ ] **Step 4: Commit**

```bash
git add components/buttons/
git commit -m "feat: add hold-both gesture detection to buttons component"
```

### Task 3: Refactor fan_control to expose button_event as public function

**Files:**
- Modify: `components/fan_control/include/fan_control.h`
- Modify: `components/fan_control/fan_control.c`

- [ ] **Step 1: Add fan_control_button_event to header**

Add to `components/fan_control/include/fan_control.h` after the existing function declarations:

```c
#include "buttons.h"

void fan_control_button_event(button_id_t btn, button_event_t evt);
```

- [ ] **Step 2: Refactor fan_control.c**

In `components/fan_control/fan_control.c`:

1. Rename `button_callback` to `fan_control_button_event` and make it public (remove `static`, change signature to drop `user_data` param).
2. Remove the `buttons_register_callback(button_callback, NULL)` call from `fan_control_init()` (line 245).

Replace the `button_callback` function (lines 171-209):

```c
void fan_control_button_event(button_id_t btn, button_event_t evt)
{
    // Ignore hold-both — handled by main.c provisioning logic
    if (evt == BTN_EVT_HOLD_BOTH) return;

    fan_command_t cmd = {
        .source = FAN_SRC_BUTTON,
        .speed = 0,
        .direction = 0,
        .mode = FAN_MODE_MANUAL,
    };

    fan_state_t state;
    fan_control_get_state(&state);

    if (!state.running) {
        if (evt == BTN_EVT_PRESS) {
            cmd.type = FAN_CMD_TURN_ON;
        } else {
            cmd.type = FAN_CMD_TURN_ON;
            cmd.speed = 20;
            if (btn == BTN_ID_SPEED) {
                cmd.direction = FAN_DIR_INTAKE;
            } else {
                cmd.direction = FAN_DIR_EXHAUST;
            }
        }
    } else {
        if (evt == BTN_EVT_HOLD) {
            cmd.type = FAN_CMD_TURN_OFF;
        } else if (btn == BTN_ID_SPEED) {
            cmd.type = FAN_CMD_SPEED_CYCLE;
        } else {
            cmd.type = FAN_CMD_DIRECTION_TOGGLE;
        }
    }

    fan_control_send_command(&cmd);
}
```

Remove from `fan_control_init()` (line 244-245):

```c
    // Remove this line:
    // buttons_register_callback(button_callback, NULL);
```

- [ ] **Step 3: Update main.c with unified button dispatcher**

In `main/main.c`, add a button callback that routes events:

```c
#include "fan_control.h"
#include "buttons.h"

static void button_dispatcher(button_id_t btn, button_event_t evt, void *user_data)
{
    if (evt == BTN_EVT_HOLD_BOTH) {
        // TODO: provisioning enter/exit (Task 6)
        ESP_LOGI(TAG, "Hold-both detected — provisioning trigger (not yet wired)");
        return;
    }
    fan_control_button_event(btn, evt);
}
```

In `app_main()`, after `buttons_init()` (line 76), register the dispatcher:

```c
    buttons_register_callback(button_dispatcher, NULL);
```

Also update the comment on `fan_control_init()` call (line 68):

```c
    // 2. Fan state machine (creates task + queue)
    ESP_ERROR_CHECK(fan_control_init());
```

And update the comment on `buttons_init()` call:

```c
    // 3. Buttons (polling task)
    ...
    ESP_ERROR_CHECK(buttons_init(&btn_cfg));

    // 4. Button dispatcher — routes events to fan_control or provisioning
    buttons_register_callback(button_dispatcher, NULL);
```

- [ ] **Step 4: Remove buttons dependency from fan_control CMakeLists**

In `components/fan_control/CMakeLists.txt`, the `buttons` dependency is no longer needed since fan_control no longer calls `buttons_register_callback`. However, `fan_control.h` now includes `buttons.h` for the types, so we need to keep it:

```cmake
idf_component_register(
    SRCS "fan_control.c"
    INCLUDE_DIRS "include"
    REQUIRES bts7960 buttons settings
)
```

No change needed — `buttons` stays because `fan_control_button_event` uses `button_id_t` and `button_event_t`.

- [ ] **Step 5: Build and verify**

Run: `./build.sh`
Expected: Clean build. Fan buttons should still work identically (press/hold behavior unchanged). Hold-both logs message but doesn't trigger provisioning yet.

- [ ] **Step 6: Commit**

```bash
git add components/fan_control/ main/main.c
git commit -m "refactor: move button dispatch to main.c, expose fan_control_button_event"
```

---

## Chunk 3: WiFi Manager Stop/Start + Event Emitter/API Lifecycle

### Task 4: Add wifi_manager_stop() and wifi_manager_start()

**Files:**
- Modify: `components/wifi/include/wifi_manager.h`
- Modify: `components/wifi/wifi_manager.c`
- Modify: `components/wifi/CMakeLists.txt`
- Modify: `sdkconfig.defaults`

**Important:** sdkconfig BT entries must be added NOW (not in Chunk 4) because `wifi_manager.c` references `wifi_prov_scheme_ble` which requires NimBLE to be enabled at build time.

- [ ] **Step 1: Update wifi_manager.h**

Replace `components/wifi/include/wifi_manager.h`:

```c
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_state_callback_t)(bool connected);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_stop(void);
esp_err_t wifi_manager_start(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_register_state_cb(wifi_state_callback_t cb);
```

- [ ] **Step 2: Add BLE config to sdkconfig.defaults**

Append to `sdkconfig.defaults` (needed before wifi_manager.c can reference `wifi_prov_scheme_ble`):

```
# Bluetooth (NimBLE for BLE provisioning)
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL=y
```

- [ ] **Step 3: Update wifi CMakeLists.txt**

Add `wifi_provisioning` dependency to `components/wifi/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif mdns esp_event wifi_provisioning
)
```

- [ ] **Step 4: Rewrite wifi_manager.c with stop/start and NVS credential check**

Replace `components/wifi/wifi_manager.c`:

```c
#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "wifi";

#define RECONNECT_BASE_MS   1000
#define RECONNECT_MAX_MS    60000
#define MAX_STATE_CBS       4

static EventGroupHandle_t s_wifi_events;
static const int CONNECTED_BIT = BIT0;

static TimerHandle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;
static bool s_suppress_reconnect = false;  // true when wifi intentionally stopped

static wifi_state_callback_t s_state_cbs[MAX_STATE_CBS];
static int s_num_state_cbs = 0;

static void notify_state(bool connected)
{
    for (int i = 0; i < s_num_state_cbs; i++) {
        if (s_state_cbs[i]) {
            s_state_cbs[i](connected);
        }
    }
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_VANFAN_MDNS_HOSTNAME);
    mdns_instance_name_set("VanFan Controller");
    mdns_service_add("VanFan Controller", "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: hostname set to %s.local", CONFIG_VANFAN_MDNS_HOSTNAME);
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    if (s_suppress_reconnect) return;
    ESP_LOGI(TAG, "Reconnecting (backoff %lums)...", (unsigned long)s_reconnect_delay_ms);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (s_suppress_reconnect) return;

    xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
    xTimerStart(s_reconnect_timer, 0);

    s_reconnect_delay_ms *= 2;
    if (s_reconnect_delay_ms > RECONNECT_MAX_MS) {
        s_reconnect_delay_ms = RECONNECT_MAX_MS;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected");
            xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);
            notify_state(false);
            schedule_reconnect();
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        notify_state(true);
        start_mdns();
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) return ESP_ERR_NO_MEM;

    s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(RECONNECT_BASE_MS),
                                      pdFALSE, NULL, reconnect_timer_cb);
    if (!s_reconnect_timer) return ESP_ERR_NO_MEM;

    // Global singletons — called once
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers (persist across stop/start since they're on the event loop)
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    // Check if credentials were provisioned via BLE (stored in NVS)
    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    bool provisioned = false;
    esp_err_t prov_ret = wifi_prov_mgr_init(prov_cfg);
    if (prov_ret == ESP_OK) {
        wifi_prov_mgr_is_provisioned(&provisioned);
        wifi_prov_mgr_deinit();
    }

    if (provisioned) {
        ESP_LOGI(TAG, "Using NVS-provisioned WiFi credentials");
        // WiFi driver reads credentials from NVS automatically (WIFI_STORAGE_FLASH default)
    } else {
        ESP_LOGI(TAG, "Using compile-time WiFi credentials");
        wifi_config_t wifi_cfg = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            },
        };
        strncpy((char *)wifi_cfg.sta.ssid, CONFIG_VANFAN_WIFI_SSID,
                sizeof(wifi_cfg.sta.ssid) - 1);
        strncpy((char *)wifi_cfg.sta.password, CONFIG_VANFAN_WIFI_PASSWORD,
                sizeof(wifi_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping WiFi...");

    // Suppress reconnect attempts
    s_suppress_reconnect = true;
    xTimerStop(s_reconnect_timer, portMAX_DELAY);

    // Tear down mDNS
    mdns_free();

    // Disconnect and deinit WiFi driver (NOT netif or event loop)
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();

    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);

    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi...");

    s_suppress_reconnect = false;
    s_reconnect_delay_ms = RECONNECT_BASE_MS;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Event handlers are still registered on the event loop — no re-registration needed
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // Credentials already in NVS (from provisioning) or set_config was called in init
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi restarted");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT) != 0;
}

esp_err_t wifi_manager_register_state_cb(wifi_state_callback_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (s_num_state_cbs >= MAX_STATE_CBS) return ESP_ERR_NO_MEM;
    s_state_cbs[s_num_state_cbs++] = cb;
    return ESP_OK;
}
```

- [ ] **Step 5: Build to verify**

Run: `./build.sh`
Expected: Clean build. Normal WiFi behavior unchanged. Binary grows due to BLE + wifi_provisioning linkage.

- [ ] **Step 6: Commit**

```bash
git add components/wifi/ sdkconfig.defaults
git commit -m "feat: add wifi_manager stop/start, NVS credential priority, enable NimBLE"
```

### Task 5: Add event_emitter stop/start and api stop/start

**Files:**
- Modify: `components/event_emitter/include/event_emitter.h`
- Modify: `components/event_emitter/event_emitter.c`
- Modify: `components/api/include/api.h`
- Modify: `components/api/api.c`

- [ ] **Step 1: Update event_emitter.h**

Add to `components/event_emitter/include/event_emitter.h`:

```c
esp_err_t event_emitter_stop(void);
esp_err_t event_emitter_start(void);
```

- [ ] **Step 2: Implement event_emitter stop/start**

Add to `components/event_emitter/event_emitter.c`:

```c
esp_err_t event_emitter_stop(void)
{
    // Stop keepalive timer to prevent sends on stale handles
    if (s_keepalive_timer) {
        xTimerStop(s_keepalive_timer, portMAX_DELAY);
    }

    // Clear all clients
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        s_clients[i].active = false;
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stopped — all SSE clients cleared");
    return ESP_OK;
}

esp_err_t event_emitter_start(void)
{
    // Restart keepalive timer (client list is empty; new SSE connections will register)
    if (s_keepalive_timer) {
        xTimerStart(s_keepalive_timer, 0);
    }

    ESP_LOGI(TAG, "Started — ready for SSE clients");
    return ESP_OK;
}
```

- [ ] **Step 3: Update api.h**

Add to `components/api/include/api.h`:

```c
esp_err_t api_stop(void);
esp_err_t api_start(void);
```

- [ ] **Step 4: Implement api stop/start**

Add to `components/api/api.c`:

```c
esp_err_t api_stop(void)
{
    // Stop event emitter BEFORE httpd to prevent stale handle access
    event_emitter_stop();

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

esp_err_t api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    int count = sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]);
    for (int i = 0; i < count; i++) {
        httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
    }

    // Restart event emitter after httpd is ready
    event_emitter_start();

    ESP_LOGI(TAG, "HTTP server restarted (%d endpoints)", count);
    return ESP_OK;
}
```

- [ ] **Step 5: Build to verify**

Run: `./build.sh`
Expected: Clean build. No behavioral change (stop/start not called yet).

- [ ] **Step 6: Commit**

```bash
git add components/event_emitter/ components/api/
git commit -m "feat: add stop/start lifecycle to event_emitter and api components"
```

---

## Chunk 4: BLE Provisioning Component

### Task 6: Create ble_prov component

**Files:**
- Create: `components/ble_prov/include/ble_prov.h`
- Create: `components/ble_prov/ble_prov.c`
- Create: `components/ble_prov/CMakeLists.txt`
- Modify: `main/CMakeLists.txt`

(Note: sdkconfig.defaults BT entries were already added in Chunk 3 Task 4.)

- [ ] **Step 1: Create ble_prov header**

Create `components/ble_prov/include/ble_prov.h`:

```c
#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*ble_prov_cb_t)(void);

esp_err_t ble_prov_init(ble_prov_cb_t on_creds_received);
esp_err_t ble_prov_start(void);
esp_err_t ble_prov_stop(void);
bool ble_prov_is_active(void);
```

- [ ] **Step 2: Create ble_prov implementation**

Create `components/ble_prov/ble_prov.c`:

```c
#include "ble_prov.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "esp_log.h"
#include "esp_event.h"

static const char *TAG = "ble_prov";

static bool s_active = false;
static ble_prov_cb_t s_on_creds_received = NULL;

static void prov_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data)
{
    if (base == WIFI_PROV_EVENT) {
        switch (id) {
        case WIFI_PROV_CRED_RECV: {
            wifi_sta_config_t *cfg = (wifi_sta_config_t *)data;
            ESP_LOGI(TAG, "Credentials received: SSID=%s", (const char *)cfg->ssid);
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning credentials verified successfully");
            // Don't restart WiFi here — wait for WIFI_PROV_END so the
            // provisioning manager fully cleans up first
            break;
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "Provisioning failed: %s",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth error" : "AP not found");
            // Stay in provisioning mode — user can retry
            break;
        }
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Provisioning ended — cleaning up");
            wifi_prov_mgr_deinit();
            s_active = false;
            // Now safe to restart WiFi — provisioning manager is fully torn down
            if (s_on_creds_received) {
                s_on_creds_received();
            }
            break;
        default:
            break;
        }
    }
}

esp_err_t ble_prov_init(ble_prov_cb_t on_creds_received)
{
    s_on_creds_received = on_creds_received;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, prov_event_handler, NULL, NULL));

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t ble_prov_start(void)
{
    ESP_LOGI(TAG, "Starting BLE provisioning...");

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BLE,
    };

    esp_err_t ret = wifi_prov_mgr_init(config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Prov mgr init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Reset provisioned flag so new credentials can be accepted
    wifi_prov_mgr_reset_provisioning();

    ret = wifi_prov_mgr_start_provisioning(
        WIFI_PROV_SECURITY_0,  // security0 — no encryption
        NULL,                   // no proof of possession
        "VANFAN",              // service name (BLE device name)
        NULL                    // no service key
    );

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start provisioning failed: %s", esp_err_to_name(ret));
        wifi_prov_mgr_deinit();
        return ret;
    }

    s_active = true;
    ESP_LOGI(TAG, "BLE advertising as 'VANFAN'");
    return ESP_OK;
}

esp_err_t ble_prov_stop(void)
{
    ESP_LOGI(TAG, "Stopping BLE provisioning...");

    wifi_prov_mgr_stop_provisioning();
    wifi_prov_mgr_deinit();
    s_active = false;

    ESP_LOGI(TAG, "BLE provisioning stopped");
    return ESP_OK;
}

bool ble_prov_is_active(void)
{
    return s_active;
}
```

- [ ] **Step 3: Create CMakeLists.txt**

Create `components/ble_prov/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "ble_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES wifi_provisioning esp_event bt
)
```

- [ ] **Step 4: Add ble_prov to main CMakeLists.txt**

Update `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ota nvs_flash spi_flash bts7960 buttons fan_control event_emitter wifi api status_led ble_prov
)
```

- [ ] **Step 5: Build to verify**

Run: `./build.sh`
Expected: Clean build. Verify total binary size < 1.5MB.

- [ ] **Step 6: Commit**

```bash
git add components/ble_prov/ main/CMakeLists.txt
git commit -m "feat: add ble_prov component wrapping wifi_provisioning with NimBLE"
```

---

## Chunk 5: Main.c Orchestration — Wire Everything Together

### Task 7: Wire provisioning logic into main.c

**Files:**
- Modify: `main/main.c`

- [ ] **Step 1: Rewrite main.c with full provisioning orchestration**

Replace `main/main.c`:

```c
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "bts7960.h"
#include "buttons.h"
#include "fan_control.h"
#include "event_emitter.h"
#include "wifi_manager.h"
#include "api.h"
#include "ota.h"
#include "status_led.h"
#include "ble_prov.h"

static const char *TAG = "vanfan";

static bool s_provisioning_active = false;
static TickType_t s_prov_enter_tick = 0;
#define PROV_GRACE_PERIOD_MS 5000

static void on_wifi_state(bool connected)
{
    if (connected) {
        status_led_set_state(STATUS_LED_OFF);
    } else if (!s_provisioning_active) {
        status_led_set_state(STATUS_LED_WIFI_DISCONNECTED);
    }
}

static void enter_provisioning(void)
{
    if (s_provisioning_active) return;

    ESP_LOGI(TAG, "=== ENTERING BLE PROVISIONING MODE ===");
    s_provisioning_active = true;
    s_prov_enter_tick = xTaskGetTickCount();

    // Shut down networking stack (order matters)
    api_stop();
    wifi_manager_stop();

    // Start BLE
    status_led_set_state(STATUS_LED_BLE_PROVISIONING);
    ble_prov_start();
}

static void on_prov_creds_received(void)
{
    // Called from WIFI_PROV_END event — provisioning manager is fully torn down,
    // BLE is stopped, wifi_prov_mgr_deinit() already called. Safe to restart WiFi.
    ESP_LOGI(TAG, "=== CREDENTIALS RECEIVED — RESTARTING WIFI ===");
    s_provisioning_active = false;

    status_led_set_state(STATUS_LED_SUCCESS);

    // Restart networking stack (WiFi driver reads new creds from NVS)
    wifi_manager_start();
    api_start();
}

static void exit_provisioning_cancel(void)
{
    ESP_LOGI(TAG, "=== PROVISIONING CANCELLED ===");

    ble_prov_stop();
    s_provisioning_active = false;

    // Restart networking with previous credentials
    wifi_manager_start();
    api_start();
    status_led_set_state(STATUS_LED_OFF);
}

static void button_dispatcher(button_id_t btn, button_event_t evt, void *user_data)
{
    if (evt == BTN_EVT_HOLD_BOTH) {
        if (s_provisioning_active) {
            // Check grace period
            TickType_t now = xTaskGetTickCount();
            uint32_t elapsed = (now - s_prov_enter_tick) * portTICK_PERIOD_MS;
            if (elapsed >= PROV_GRACE_PERIOD_MS) {
                exit_provisioning_cancel();
            } else {
                ESP_LOGI(TAG, "Hold-both ignored — grace period (%lums remaining)",
                         (unsigned long)(PROV_GRACE_PERIOD_MS - elapsed));
            }
        } else {
            enter_provisioning();
        }
        return;
    }

    // During provisioning, single presses still control fan
    fan_control_button_event(btn, evt);
}

void app_main(void)
{
    // Wait for USB Serial/JTAG to reconnect after reset
    vTaskDelay(pdMS_TO_TICKS(3000));

    // NVS init
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Status LED (early, so we can show state during boot)
    ESP_ERROR_CHECK(status_led_init());

    // Version
    const esp_app_desc_t *app_desc = esp_app_get_description();
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "  VanFan Controller v%s", app_desc->version);
    ESP_LOGI(TAG, "================================");

    // Chip info
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "Chip: %s, cores: %d, revision: v%d.%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             chip_info.revision / 100, chip_info.revision % 100);

    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash: %luMB %s", flash_size / (1024 * 1024),
                 (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    }

    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());

    // 1. Motor driver hardware
    bts7960_config_t motor_cfg = {
        .rpwm_gpio = CONFIG_VANFAN_PIN_RPWM,
        .lpwm_gpio = CONFIG_VANFAN_PIN_LPWM,
        .r_en_gpio = CONFIG_VANFAN_PIN_R_EN,
        .l_en_gpio = CONFIG_VANFAN_PIN_L_EN,
        .r_is_gpio = CONFIG_VANFAN_PIN_R_IS,
        .l_is_gpio = CONFIG_VANFAN_PIN_L_IS,
    };
    ESP_ERROR_CHECK(bts7960_init(&motor_cfg));

    // 2. Fan state machine (creates task + queue)
    ESP_ERROR_CHECK(fan_control_init());

    // 3. Buttons (polling task)
    buttons_config_t btn_cfg = {
        .speed_gpio = CONFIG_VANFAN_PIN_BTN_SPEED,
        .direction_gpio = CONFIG_VANFAN_PIN_BTN_DIRECTION,
    };
    ESP_ERROR_CHECK(buttons_init(&btn_cfg));

    // 4. Button dispatcher — routes events to fan_control or provisioning
    buttons_register_callback(button_dispatcher, NULL);

    // 5. Event emitter (registers state change callback for SSE)
    ESP_ERROR_CHECK(event_emitter_init());

    // 6. BLE provisioning (init only — started on hold-both trigger)
    ESP_ERROR_CHECK(ble_prov_init(on_prov_creds_received));

    // 7. WiFi (non-blocking — buttons work before WiFi connects)
    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_register_state_cb(on_wifi_state);

    // 8. HTTP API server
    ESP_ERROR_CHECK(api_init());

    // 9. OTA boot validation (mark firmware valid if pending verify)
    ESP_ERROR_CHECK(ota_init());

    ESP_LOGI(TAG, "Startup complete. Fan off, awaiting input.");

    // Heartbeat
    while (1) {
        fan_state_t state;
        fan_control_get_state(&state);
        ESP_LOGI(TAG, "heartbeat | heap=%lu running=%d speed=%d dir=%s output=%d wifi=%s prov=%s",
                 (unsigned long)esp_get_free_heap_size(),
                 state.running, state.speed_percent,
                 state.direction == FAN_DIR_EXHAUST ? "exhaust" : "intake",
                 bts7960_get_current_output(),
                 wifi_manager_is_connected() ? "yes" : "no",
                 s_provisioning_active ? "BLE" : "off");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
```

- [ ] **Step 2: Build to verify**

Run: `./build.sh`
Expected: Clean build. Check binary size is < 1.5MB.

- [ ] **Step 3: Commit**

```bash
git add main/main.c
git commit -m "feat: wire BLE provisioning orchestration into main.c"
```

---

## Chunk 6: Web Bluetooth Provisioning Page

### Task 8: Create Chrome Web Bluetooth provisioning page

**Files:**
- Create: `tools/provision.html`

- [ ] **Step 1: Create provision.html**

Create `tools/provision.html`. This page uses the Web Bluetooth API to connect to the ESP32's `wifi_provisioning` BLE GATT service and send WiFi credentials using the protocomm security0 protocol.

The ESP-IDF `wifi_provisioning` component with BLE transport exposes:
- A custom BLE service with name-based characteristics
- Characteristics: `prov-session` (session establishment), `prov-config` (credential exchange), `proto-ver` (protocol version)
- Security 0 means the session message is a simple no-op (session response with status OK)
- Credential data uses protobuf encoding (`wifi_config.proto` schema)

```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>VanFan WiFi Provisioning</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif;
            background: #1a1a2e;
            color: #e0e0e0;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .container {
            background: #16213e;
            border-radius: 12px;
            padding: 32px;
            width: 400px;
            max-width: 90vw;
            box-shadow: 0 8px 32px rgba(0,0,0,0.3);
        }
        h1 {
            font-size: 20px;
            margin-bottom: 4px;
            color: #fff;
        }
        .subtitle { color: #888; font-size: 13px; margin-bottom: 24px; }
        .status {
            padding: 10px 14px;
            border-radius: 8px;
            margin-bottom: 20px;
            font-size: 13px;
            display: none;
        }
        .status.info { display: block; background: #0a3d62; color: #82ccdd; }
        .status.success { display: block; background: #1e4d2b; color: #7dcea0; }
        .status.error { display: block; background: #4a1a1a; color: #e74c3c; }
        label {
            display: block;
            font-size: 13px;
            color: #aaa;
            margin-bottom: 4px;
        }
        input {
            width: 100%;
            padding: 10px 12px;
            border: 1px solid #2a2a4a;
            border-radius: 6px;
            background: #0f0f23;
            color: #fff;
            font-size: 14px;
            margin-bottom: 16px;
            outline: none;
        }
        input:focus { border-color: #4a90d9; }
        button {
            width: 100%;
            padding: 12px;
            border: none;
            border-radius: 8px;
            font-size: 14px;
            font-weight: 600;
            cursor: pointer;
            transition: background 0.2s;
        }
        button:disabled { opacity: 0.5; cursor: not-allowed; }
        .btn-connect { background: #4a90d9; color: #fff; margin-bottom: 12px; }
        .btn-connect:hover:not(:disabled) { background: #357abd; }
        .btn-provision { background: #27ae60; color: #fff; }
        .btn-provision:hover:not(:disabled) { background: #1e8449; }
        .btn-disconnect { background: #555; color: #fff; margin-top: 12px; font-size: 12px; padding: 8px; }
        .btn-disconnect:hover:not(:disabled) { background: #777; }
        .connected-info {
            font-size: 12px;
            color: #7dcea0;
            margin-bottom: 16px;
            padding: 8px;
            background: #1e4d2b33;
            border-radius: 6px;
        }
        .hidden { display: none !important; }
    </style>
</head>
<body>
    <div class="container">
        <h1>VanFan Provisioning</h1>
        <p class="subtitle">Send WiFi credentials via Bluetooth</p>

        <div id="status" class="status"></div>

        <!-- Connect phase -->
        <div id="connect-section">
            <button id="btn-connect" class="btn-connect" onclick="connectDevice()">
                Connect to VanFan
            </button>
        </div>

        <!-- Provision phase (shown after BLE connect) -->
        <div id="provision-section" class="hidden">
            <div id="device-info" class="connected-info"></div>

            <label for="ssid">WiFi Network (SSID)</label>
            <input type="text" id="ssid" placeholder="Enter WiFi SSID" autocomplete="off">

            <label for="password">WiFi Password</label>
            <input type="password" id="password" placeholder="Enter WiFi password">

            <button id="btn-provision" class="btn-provision" onclick="provision()">
                Send Credentials
            </button>

            <button class="btn-disconnect" onclick="disconnect()">
                Disconnect
            </button>
        </div>
    </div>

    <script>
    // ESP-IDF wifi_provisioning BLE GATT constants
    // The service UUID and characteristic UUIDs are based on the service name
    // For the default "prov" endpoint naming used by wifi_provisioning:

    let device = null;
    let server = null;
    let service = null;
    let sessionChar = null;
    let configChar = null;

    // Protocomm uses custom 128-bit UUIDs based on the service name.
    // ESP-IDF wifi_provisioning with BLE generates these from the device name.
    // The primary service UUID for name-based BLE provisioning:
    // 0000FFFF-0000-1000-8000-00805F9B34FB (16-bit: 0xFFFF)
    // But the actual UUIDs depend on the endpoint names. For the default:
    // proto-ver, prov-session, prov-config, prov-scan (if enabled)

    // Helper: encode a string to Uint8Array
    function strToBytes(str) {
        return new TextEncoder().encode(str);
    }

    // Helper: encode a protobuf varint
    function encodeVarint(value) {
        const bytes = [];
        while (value > 0x7f) {
            bytes.push((value & 0x7f) | 0x80);
            value >>>= 7;
        }
        bytes.push(value & 0x7f);
        return new Uint8Array(bytes);
    }

    // Helper: encode a protobuf length-delimited field
    function encodeBytes(fieldNumber, data) {
        const tag = encodeVarint((fieldNumber << 3) | 2);
        const len = encodeVarint(data.length);
        const result = new Uint8Array(tag.length + len.length + data.length);
        result.set(tag, 0);
        result.set(len, tag.length);
        result.set(data, tag.length + len.length);
        return result;
    }

    // Helper: encode a protobuf varint field
    function encodeVarintField(fieldNumber, value) {
        const tag = encodeVarint((fieldNumber << 3) | 0);
        const val = encodeVarint(value);
        const result = new Uint8Array(tag.length + val.length);
        result.set(tag, 0);
        result.set(val, tag.length);
        return result;
    }

    // Helper: concatenate Uint8Arrays
    function concatBytes(...arrays) {
        const total = arrays.reduce((sum, a) => sum + a.length, 0);
        const result = new Uint8Array(total);
        let offset = 0;
        for (const a of arrays) {
            result.set(a, offset);
            offset += a.length;
        }
        return result;
    }

    // Build Security0 session request (SessionCmd with Sec0Payload)
    // SessionCmd { sec_ver: 0, sec0: S0SessionCmd { } }
    // Proto: message SessionCmd { SecSchemeVersion sec_ver = 2; oneof { Sec0Payload S0 = 10; } }
    // S0SessionCmd is empty for security0
    function buildSessionRequest() {
        // Sec0Payload message: S0SessionCmd { } — it's the "command" variant
        // Sec0Payload { msg: S0_Session_Command = 0, sc: S0SessionCmd {} }
        const s0Cmd = new Uint8Array(0); // empty S0SessionCmd

        // Sec0Payload: msg (field 1, enum 0 = S0_Session_Command), sc (field 20, S0SessionCmd)
        const sec0Payload = encodeVarintField(1, 0); // msg = S0_Session_Command = 0
        // sc field is field 20 (S0SessionCmd) — empty, so we can skip it

        // SessionCmd: sec_ver (field 2, enum 0 = SecScheme0), proto.S0 (field 10)
        const secVer = encodeVarintField(2, 0); // sec_ver = SecScheme0 = 0
        const s0Field = encodeBytes(10, sec0Payload); // S0 = Sec0Payload
        return concatBytes(secVer, s0Field);
    }

    // Build WiFi config set request
    // CmdSetConfig { ssid: bytes, passphrase: bytes }
    // WiFiConfigPayload { msg: TypeCmdSetConfig = 0, cmd_set_config: CmdSetConfig }
    function buildConfigRequest(ssid, password) {
        // CmdSetConfig: ssid (field 1), passphrase (field 2)
        const cmdSetConfig = concatBytes(
            encodeBytes(1, strToBytes(ssid)),
            encodeBytes(2, strToBytes(password))
        );

        // WiFiConfigPayload: msg (field 1, enum 0 = TypeCmdSetConfig),
        //                     cmd_set_config (field 10)
        return concatBytes(
            encodeVarintField(1, 0), // msg = TypeCmdSetConfig = 0
            encodeBytes(10, cmdSetConfig) // cmd_set_config
        );
    }

    // Build WiFi apply config request
    // WiFiConfigPayload { msg: TypeCmdApplyConfig = 2 }
    function buildApplyRequest() {
        return encodeVarintField(1, 2); // msg = TypeCmdApplyConfig = 2
    }

    function setStatus(msg, type) {
        const el = document.getElementById('status');
        el.textContent = msg;
        el.className = 'status ' + type;
    }

    async function connectDevice() {
        const btn = document.getElementById('btn-connect');
        btn.disabled = true;
        btn.textContent = 'Scanning...';
        setStatus('Looking for VanFan device...', 'info');

        try {
            // Request BLE device — filter by name prefix
            device = await navigator.bluetooth.requestDevice({
                filters: [{ namePrefix: 'PROV_VANFAN' }],
                optionalServices: ['0000ffff-0000-1000-8000-00805f9b34fb']
            });

            setStatus('Connecting to ' + device.name + '...', 'info');
            server = await device.gatt.connect();

            // Discover services
            const services = await server.getPrimaryServices();
            service = services[0]; // The provisioning service

            // Discover characteristics
            const chars = await service.getCharacteristics();

            for (const c of chars) {
                // Read the descriptor to identify the endpoint name
                // ESP-IDF encodes endpoint names in characteristic user descriptions
                try {
                    const descriptors = await c.getDescriptors();
                    for (const d of descriptors) {
                        if (d.uuid === '00002901-0000-1000-8000-00805f9b34fb') {
                            const val = await d.readValue();
                            const name = new TextDecoder().decode(val);
                            if (name === 'prov-session') sessionChar = c;
                            else if (name === 'prov-config') configChar = c;
                        }
                    }
                } catch (e) {
                    // Some characteristics may not have descriptors
                }
            }

            // Fallback: if descriptors didn't work, try by characteristic order
            // ESP-IDF typically creates characteristics in order: proto-ver, prov-session, prov-config
            if (!sessionChar || !configChar) {
                const sortedChars = chars.sort((a, b) => a.uuid.localeCompare(b.uuid));
                if (sortedChars.length >= 3) {
                    sessionChar = sortedChars[1]; // prov-session
                    configChar = sortedChars[2];  // prov-config
                } else if (sortedChars.length >= 2) {
                    sessionChar = sortedChars[0];
                    configChar = sortedChars[1];
                }
            }

            if (!sessionChar || !configChar) {
                throw new Error('Could not find provisioning characteristics');
            }

            // Establish security0 session
            const sessionReq = buildSessionRequest();
            await sessionChar.writeValueWithResponse(sessionReq);
            const sessionResp = await sessionChar.readValue();
            // For security0, any response means session is established

            setStatus('Connected to ' + device.name, 'success');
            document.getElementById('connect-section').classList.add('hidden');
            document.getElementById('provision-section').classList.remove('hidden');
            document.getElementById('device-info').textContent =
                'Connected to: ' + device.name;
            document.getElementById('ssid').focus();

        } catch (e) {
            setStatus('Connection failed: ' + e.message, 'error');
            btn.disabled = false;
            btn.textContent = 'Connect to VanFan';
        }
    }

    async function provision() {
        const ssid = document.getElementById('ssid').value.trim();
        const password = document.getElementById('password').value;
        const btn = document.getElementById('btn-provision');

        if (!ssid) {
            setStatus('Please enter a WiFi SSID', 'error');
            return;
        }

        btn.disabled = true;
        btn.textContent = 'Sending...';
        setStatus('Sending WiFi credentials...', 'info');

        try {
            // Step 1: Send WiFi config (SSID + password)
            const configReq = buildConfigRequest(ssid, password);
            await configChar.writeValueWithResponse(configReq);
            const configResp = await configChar.readValue();

            // Step 2: Apply config (tells ESP32 to connect)
            const applyReq = buildApplyRequest();
            await configChar.writeValueWithResponse(applyReq);
            const applyResp = await configChar.readValue();

            setStatus('Credentials sent! VanFan is connecting to "' + ssid + '"...', 'success');
            btn.textContent = 'Done!';

            // Disconnect BLE after a short delay
            setTimeout(() => {
                if (server && server.connected) {
                    server.disconnect();
                }
            }, 2000);

        } catch (e) {
            setStatus('Provisioning failed: ' + e.message, 'error');
            btn.disabled = false;
            btn.textContent = 'Send Credentials';
        }
    }

    function disconnect() {
        if (server && server.connected) {
            server.disconnect();
        }
        device = null;
        server = null;
        service = null;
        sessionChar = null;
        configChar = null;

        document.getElementById('connect-section').classList.remove('hidden');
        document.getElementById('provision-section').classList.add('hidden');
        document.getElementById('btn-connect').disabled = false;
        document.getElementById('btn-connect').textContent = 'Connect to VanFan';
        setStatus('Disconnected', 'info');
    }

    // Handle unexpected disconnection
    if (navigator.bluetooth) {
        // Check if Web Bluetooth is available
        navigator.bluetooth.getAvailability().then(available => {
            if (!available) {
                setStatus('Web Bluetooth is not available in this browser. Use Chrome.', 'error');
                document.getElementById('btn-connect').disabled = true;
            }
        });
    } else {
        setStatus('Web Bluetooth is not supported. Use Chrome on macOS, Linux, or Android.', 'error');
        document.getElementById('btn-connect').disabled = true;
    }
    </script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add tools/provision.html
git commit -m "feat: add Chrome Web Bluetooth provisioning page"
```

---

## Chunk 7: Build, Flash, and Integration Test

### Task 9: Full build and hardware test

- [ ] **Step 1: Clean build**

```bash
# Delete any cached sdkconfig (force regeneration from defaults)
rm -f sdkconfig
./build.sh
```

Expected: Clean build. Check binary size in output — should be < 1.5MB.

- [ ] **Step 2: Flash firmware**

```bash
esptool.py --chip esp32s3 --port /dev/cu.usbmodem* write_flash \
    0x9000 firmware/nvs.bin \
    0xf000 firmware/ota_data_initial.bin \
    0x20000 firmware/vanfan.bin
```

- [ ] **Step 3: Verify normal boot**

Monitor serial output. Expected:
- Status LED off (or brief flash during init)
- WiFi connects with compile-time credentials
- API accessible at `http://vanfan.local/api/v1/status`
- Fan buttons work normally (speed cycle, direction toggle, hold to off)

- [ ] **Step 4: Test hold-both → BLE provisioning entry**

Hold both buttons for 800ms. Expected:
- Serial log: "ENTERING BLE PROVISIONING MODE"
- Status LED: solid blue
- WiFi disconnects, HTTP server stops
- Fan continues running if it was on

- [ ] **Step 5: Test Web Bluetooth provisioning**

1. Open `tools/provision.html` in Chrome
2. Click "Connect to VanFan"
3. Select "PROV_VANFAN" from the Bluetooth device picker
4. Enter WiFi SSID and password
5. Click "Send Credentials"

Expected:
- LED flashes green (2s)
- WiFi connects with new credentials
- API accessible again
- Serial log: "CREDENTIALS RECEIVED — RESTARTING WIFI"

- [ ] **Step 6: Verify credentials persist across reboot**

Reset the ESP32. Expected:
- Serial log: "Using NVS-provisioned WiFi credentials"
- Connects to WiFi without compile-time defaults

- [ ] **Step 7: Test manual cancel**

1. Hold both buttons → enters provisioning (blue LED)
2. Wait 5+ seconds
3. Hold both buttons again → exits provisioning
4. Expected: WiFi reconnects with previous credentials, LED off

- [ ] **Step 8: Verify fan control during provisioning**

While in BLE provisioning mode:
- Press speed button → speed cycles
- Press direction button → direction toggles
- Hold single button → fan turns off
- None of these should exit provisioning mode

- [ ] **Step 9: Commit final state**

```bash
git add -A
git commit -m "feat: BLE WiFi provisioning — integration tested"
```
