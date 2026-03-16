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
#include "led_control.h"
#include "light_button.h"
#include "driver/gpio.h"

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
    wifi_manager_suspend();

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

static void light_button_dispatcher(light_button_event_t evt, void *user_data)
{
    switch (evt) {
    case LIGHT_BTN_EVT_PRESS: {
        int action = led_control_get_button_action();
        if (action == LED_BTN_ACTION_ALL) {
            led_control_toggle_all(LED_SRC_BUTTON);
        } else {
            led_control_toggle_zone(action, LED_SRC_BUTTON);
        }
        break;
    }
    case LIGHT_BTN_EVT_HOLD:
        led_control_all_off(LED_SRC_BUTTON);
        break;
    }
}

static bool check_wifi_reset_button(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_VANFAN_PIN_BTN_LIGHT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Check if button is held low at boot
    if (gpio_get_level(CONFIG_VANFAN_PIN_BTN_LIGHT) != 0) {
        return false;
    }

    ESP_LOGW(TAG, "Light button held at boot — waiting 2s to confirm WiFi reset...");

    // Poll for 2 seconds to confirm deliberate hold
    for (int i = 0; i < 40; i++) {  // 40 x 50ms = 2s
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(CONFIG_VANFAN_PIN_BTN_LIGHT) != 0) {
            ESP_LOGI(TAG, "Button released early — no WiFi reset");
            return false;
        }
    }

    return true;
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

    // Boot-time WiFi credential reset (hold light button during power-on)
    if (check_wifi_reset_button()) {
        ESP_LOGW(TAG, "========================================");
        ESP_LOGW(TAG, "  === WIFI CREDENTIAL RESET ===");
        ESP_LOGW(TAG, "========================================");
        status_led_set_state(STATUS_LED_WIFI_RESET);
        wifi_manager_request_credential_reset();
    }

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

    // 5. LED lighting (3-zone PWM)
    led_control_config_t led_cfg = {
        .zone_gpios = {
            CONFIG_VANFAN_PIN_LED_ZONE1,
            CONFIG_VANFAN_PIN_LED_ZONE2,
            CONFIG_VANFAN_PIN_LED_ZONE3,
        },
    };
    ESP_ERROR_CHECK(led_control_init(&led_cfg));

    // 6. Light button (polling task)
    light_button_config_t light_btn_cfg = {
        .gpio = CONFIG_VANFAN_PIN_BTN_LIGHT,
    };
    ESP_ERROR_CHECK(light_button_init(&light_btn_cfg));
    light_button_register_callback(light_button_dispatcher, NULL);

    // 7. Event emitter (registers state change callback for SSE)
    ESP_ERROR_CHECK(event_emitter_init());

    // 8. WiFi (creates event loop + netif — must be before ble_prov_init)
    ESP_ERROR_CHECK(wifi_manager_init());
    wifi_manager_register_state_cb(on_wifi_state);

    // 9. BLE provisioning (init only — registers event handler, started on hold-both)
    ESP_ERROR_CHECK(ble_prov_init(on_prov_creds_received));

    // 10. HTTP API server
    ESP_ERROR_CHECK(api_init());

    // 11. OTA boot validation (mark firmware valid if pending verify)
    ESP_ERROR_CHECK(ota_init());

    ESP_LOGI(TAG, "Startup complete. Fan off, awaiting input.");

    // Heartbeat
    while (1) {
        fan_state_t fan;
        fan_control_get_state(&fan);
        led_state_t lights;
        led_control_get_state(&lights);
        ESP_LOGI(TAG, "heartbeat | heap=%lu fan=%d/%d/%s output=%d lights=%d,%d,%d wifi=%s prov=%s",
                 (unsigned long)esp_get_free_heap_size(),
                 fan.running, fan.speed_percent,
                 fan.direction == FAN_DIR_EXHAUST ? "exhaust" : "intake",
                 bts7960_get_current_output(),
                 lights.zones[0].on ? lights.zones[0].brightness : 0,
                 lights.zones[1].on ? lights.zones[1].brightness : 0,
                 lights.zones[2].on ? lights.zones[2].brightness : 0,
                 wifi_manager_is_connected() ? "yes" : "no",
                 s_provisioning_active ? "BLE" : "off");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
