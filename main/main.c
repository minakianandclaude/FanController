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
#include "fan_control.h"
#include "event_emitter.h"
#include "wifi_manager.h"
#include "api.h"
#include "ota.h"
#include "esp_app_desc.h"

static const char *TAG = "vanfan";

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

    // 2. Fan state machine (creates task + queue, registers button callback)
    ESP_ERROR_CHECK(fan_control_init());

    // 3. Buttons (polling task — callback already registered by fan_control)
    buttons_config_t btn_cfg = {
        .speed_gpio = CONFIG_VANFAN_PIN_BTN_SPEED,
        .direction_gpio = CONFIG_VANFAN_PIN_BTN_DIRECTION,
    };
    ESP_ERROR_CHECK(buttons_init(&btn_cfg));

    // 4. Event emitter (registers state change callback for SSE)
    ESP_ERROR_CHECK(event_emitter_init());

    // 5. WiFi (non-blocking — buttons work before WiFi connects)
    ESP_ERROR_CHECK(wifi_manager_init());

    // 6. HTTP API server
    ESP_ERROR_CHECK(api_init());

    // 7. OTA boot validation (mark firmware valid if pending verify)
    ESP_ERROR_CHECK(ota_init());

    ESP_LOGI(TAG, "Startup complete. Fan off, awaiting input.");

    // Heartbeat
    while (1) {
        fan_state_t state;
        fan_control_get_state(&state);
        ESP_LOGI(TAG, "heartbeat | heap=%lu running=%d speed=%d dir=%s output=%d wifi=%s",
                 (unsigned long)esp_get_free_heap_size(),
                 state.running, state.speed_percent,
                 state.direction == FAN_DIR_EXHAUST ? "exhaust" : "intake",
                 bts7960_get_current_output(),
                 wifi_manager_is_connected() ? "yes" : "no");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
