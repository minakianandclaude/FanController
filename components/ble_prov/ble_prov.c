#include "ble_prov.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_bt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_prov";

static bool s_active = false;
static bool s_creds_verified = false;
static bool s_restart_pending = false;
static ble_prov_cb_t s_on_creds_received = NULL;

static void restart_provisioning_task(void *arg);


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
            s_creds_verified = true;
            break;
        case WIFI_PROV_CRED_FAIL: {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)data;
            ESP_LOGE(TAG, "Provisioning failed: %s — will restart for retry",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "auth error" : "AP not found");
            // Prov manager won't accept new creds after failure. Flag a restart
            // so WIFI_PROV_END handler re-launches provisioning.
            s_restart_pending = true;
            break;
        }
        case WIFI_PROV_END:
            ESP_LOGI(TAG, "Provisioning ended — cleaning up");
            wifi_prov_mgr_deinit();
            s_active = false;

            if (s_creds_verified && s_on_creds_received) {
                // Success path — release BLE memory and restart WiFi
                s_creds_verified = false;
                s_restart_pending = false;
                esp_bt_controller_disable();
                esp_bt_controller_deinit();
                esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
                ESP_LOGI(TAG, "BLE memory released");
                s_on_creds_received();
            } else if (s_restart_pending) {
                // Credential failure — restart prov manager for retry.
                // Must defer to a task because we're in the event handler
                // and can't re-init the prov manager synchronously.
                s_restart_pending = false;
                xTaskCreate(restart_provisioning_task, "prov_restart",
                            4096, NULL, 5, NULL);
            }
            // Cancel path (ble_prov_stop): neither flag set, do nothing
            break;
        default:
            break;
        }
    }
}

static void restart_provisioning_task(void *arg)
{
    ESP_LOGI(TAG, "Restarting provisioning for retry...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ble_prov_start();
    vTaskDelete(NULL);
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
        // Don't use FREE_BLE — it permanently releases BLE memory,
        // preventing restart after credential failure.
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
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

    s_creds_verified = false;
    s_restart_pending = false;
    // stop_provisioning triggers WIFI_PROV_END event, which calls
    // wifi_prov_mgr_deinit() and sets s_active=false in the handler.
    wifi_prov_mgr_stop_provisioning();

    // Release BLE resources
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    ESP_LOGI(TAG, "BLE provisioning stopped, BLE memory released");
    return ESP_OK;
}

bool ble_prov_is_active(void)
{
    return s_active;
}
