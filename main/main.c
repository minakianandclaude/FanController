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
#include "bts7960.h"
#include "buttons.h"

static const char *TAG = "vanfan";

// Simple fan state — moves to fan_control component in Phase 3
static bool fan_running = false;
static int fan_speed = 20;       // 20/40/60/80/100
static int8_t fan_direction = 1; // +1=exhaust, -1=intake

static void button_callback(button_id_t btn, button_event_t evt, void *user_data)
{
    if (!fan_running) {
        // Fan is OFF
        if (evt == BTN_EVT_PRESS) {
            // Either button press when off: turn on with last config
            fan_running = true;
            bts7960_set_output((int8_t)(fan_speed * fan_direction));
            ESP_LOGI(TAG, "ON: speed=%d dir=%s",
                     fan_speed, fan_direction > 0 ? "exhaust" : "intake");
        } else {
            // Hold when off: quick-start shortcuts
            fan_running = true;
            fan_speed = 20;
            if (btn == BTN_ID_SPEED) {
                fan_direction = -1; // hold speed = intake
            } else {
                fan_direction = 1;  // hold direction = exhaust
            }
            bts7960_set_output((int8_t)(fan_speed * fan_direction));
            ESP_LOGI(TAG, "ON (hold shortcut): speed=%d dir=%s",
                     fan_speed, fan_direction > 0 ? "exhaust" : "intake");
        }
    } else {
        // Fan is ON
        if (evt == BTN_EVT_HOLD) {
            // Hold either button: turn off
            fan_running = false;
            bts7960_set_output(0);
            ESP_LOGI(TAG, "OFF (ramp to 0)");
        } else if (btn == BTN_ID_SPEED) {
            // Speed cycle: 20->40->60->80->100->20
            if (fan_speed < 40) fan_speed = 40;
            else if (fan_speed < 60) fan_speed = 60;
            else if (fan_speed < 80) fan_speed = 80;
            else if (fan_speed < 100) fan_speed = 100;
            else fan_speed = 20;
            bts7960_set_output((int8_t)(fan_speed * fan_direction));
            ESP_LOGI(TAG, "SPEED: %d%%", fan_speed);
        } else {
            // Direction toggle
            fan_direction = -fan_direction;
            bts7960_set_output((int8_t)(fan_speed * fan_direction));
            ESP_LOGI(TAG, "DIR: %s", fan_direction > 0 ? "exhaust" : "intake");
        }
    }
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

    // Version
    ESP_LOGI(TAG, "================================");
    ESP_LOGI(TAG, "  VanFan Controller v0.1.0");
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

    // Init BTS7960 motor driver
    bts7960_config_t motor_cfg = {
        .rpwm_gpio = CONFIG_VANFAN_PIN_RPWM,
        .lpwm_gpio = CONFIG_VANFAN_PIN_LPWM,
        .r_en_gpio = CONFIG_VANFAN_PIN_R_EN,
        .l_en_gpio = CONFIG_VANFAN_PIN_L_EN,
        .r_is_gpio = CONFIG_VANFAN_PIN_R_IS,
        .l_is_gpio = CONFIG_VANFAN_PIN_L_IS,
    };
    ESP_ERROR_CHECK(bts7960_init(&motor_cfg));

    // Init buttons
    buttons_config_t btn_cfg = {
        .speed_gpio = CONFIG_VANFAN_PIN_BTN_SPEED,
        .direction_gpio = CONFIG_VANFAN_PIN_BTN_DIRECTION,
    };
    ESP_ERROR_CHECK(buttons_init(&btn_cfg));
    buttons_register_callback(button_callback, NULL);

    ESP_LOGI(TAG, "Startup complete. Fan off, awaiting button input.");
    ESP_LOGI(TAG, "Default: speed=%d dir=%s",
             fan_speed, fan_direction > 0 ? "exhaust" : "intake");

    // Heartbeat
    while (1) {
        ESP_LOGI(TAG, "heartbeat | heap=%lu running=%d speed=%d dir=%s output=%d",
                 (unsigned long)esp_get_free_heap_size(),
                 fan_running, fan_speed,
                 fan_direction > 0 ? "exhaust" : "intake",
                 bts7960_get_current_output());
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
