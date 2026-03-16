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
#define MAX_SCAN_APS        50

static EventGroupHandle_t s_wifi_events;
static const int CONNECTED_BIT = BIT0;

static TimerHandle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;
static bool s_suppress_reconnect = false;
static bool s_reconnecting = false;
static bool s_scanning = false;

static wifi_state_callback_t s_state_cbs[MAX_STATE_CBS];
static int s_num_state_cbs = 0;

// Credential management
static wifi_cred_source_t s_active_source = WIFI_CRED_SOURCE_BUILD_TIME;
static bool s_has_nvs_creds = false;
static char s_nvs_ssid[33] = {0};
static char s_nvs_password[65] = {0};
static bool s_reset_requested = false;

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
    mdns_free();  // safe if not initialized; prevents double-init on reconnect
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

// Load build-time credentials into WiFi config and connect
static void use_buildtime_creds(void)
{
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_VANFAN_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_VANFAN_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    s_active_source = WIFI_CRED_SOURCE_BUILD_TIME;
}

// Load cached NVS credentials into WiFi config and connect
static void use_nvs_creds(void)
{
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, s_nvs_ssid,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, s_nvs_password,
            sizeof(wifi_cfg.sta.password) - 1);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    s_active_source = WIFI_CRED_SOURCE_NVS;
}

static void schedule_reconnect(void);

// Scan visible networks and connect to the best known match
static void scan_and_connect(void)
{
    if (s_suppress_reconnect) return;
    if (s_scanning) {
        ESP_LOGD(TAG, "Scan already in progress — skipping");
        return;
    }
    s_scanning = true;

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s — will retry", esp_err_to_name(err));
        s_scanning = false;
        schedule_reconnect();
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count == 0) {
        ESP_LOGW(TAG, "Scan found 0 APs — will retry");
        esp_wifi_scan_get_ap_records(&ap_count, NULL);  // clear scan results
        s_scanning = false;
        schedule_reconnect();
        return;
    }

    uint16_t total_found = ap_count;
    if (ap_count > MAX_SCAN_APS) ap_count = MAX_SCAN_APS;
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to alloc scan results");
        uint16_t zero = 0;
        esp_wifi_scan_get_ap_records(&zero, NULL);
        s_scanning = false;
        schedule_reconnect();
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    ESP_LOGI(TAG, "Scan found %d APs (checked %d):", total_found, ap_count);
    for (int i = 0; i < ap_count; i++) {
        ESP_LOGI(TAG, "  [%2d] '%s' ch=%d rssi=%d", i,
                 (char *)ap_records[i].ssid,
                 ap_records[i].primary,
                 ap_records[i].rssi);
    }

    ESP_LOGI(TAG, "Known creds — NVS: %s '%s', build-time: '%s'",
             s_has_nvs_creds ? "yes" : "no",
             s_has_nvs_creds ? s_nvs_ssid : "",
             CONFIG_VANFAN_WIFI_SSID);

    // Check NVS creds first (highest priority), then build-time
    // Skip NVS match if NVS SSID is the same as build-time (prefer build-time label)
    bool found = false;

    if (s_has_nvs_creds && strcmp(s_nvs_ssid, CONFIG_VANFAN_WIFI_SSID) != 0) {
        for (int i = 0; i < ap_count; i++) {
            if (strcmp((char *)ap_records[i].ssid, s_nvs_ssid) == 0) {
                ESP_LOGI(TAG, "Matched '%s' (NVS) — connecting", s_nvs_ssid);
                use_nvs_creds();
                found = true;
                break;
            }
        }
    }

    if (!found) {
        for (int i = 0; i < ap_count; i++) {
            if (strcmp((char *)ap_records[i].ssid, CONFIG_VANFAN_WIFI_SSID) == 0) {
                ESP_LOGI(TAG, "Matched '%s' (build-time) — connecting", CONFIG_VANFAN_WIFI_SSID);
                use_buildtime_creds();
                found = true;
                break;
            }
        }
    }

    // If build-time didn't match either, try NVS even if same SSID
    if (!found && s_has_nvs_creds) {
        for (int i = 0; i < ap_count; i++) {
            if (strcmp((char *)ap_records[i].ssid, s_nvs_ssid) == 0) {
                ESP_LOGI(TAG, "Matched '%s' (NVS fallback) — connecting", s_nvs_ssid);
                use_nvs_creds();
                found = true;
                break;
            }
        }
    }

    free(ap_records);

    s_scanning = false;

    if (found) {
        err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect failed: %s — will retry", esp_err_to_name(err));
            schedule_reconnect();
        }
    } else {
        ESP_LOGW(TAG, "No known SSID visible — will retry");
        schedule_reconnect();
    }
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    if (s_suppress_reconnect) return;
    ESP_LOGI(TAG, "Reconnecting (backoff %lums)...", (unsigned long)s_reconnect_delay_ms);
    scan_and_connect();
    s_reconnecting = false;
}

static void schedule_reconnect(void)
{
    if (s_suppress_reconnect) return;
    if (s_reconnecting) return;  // already scheduled

    s_reconnecting = true;

    if (xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_reconnect_delay_ms), 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to change reconnect timer period");
        s_reconnecting = false;
        return;
    }
    if (xTimerStart(s_reconnect_timer, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to start reconnect timer");
        s_reconnecting = false;
        return;
    }

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
            if (!s_suppress_reconnect) {
                ESP_LOGI(TAG, "STA started — scanning for known networks...");
                scan_and_connect();
            }
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
        ESP_LOGI(TAG, "Got IP: " IPSTR " (source: %s)",
                 IP2STR(&event->ip_info.ip),
                 s_active_source == WIFI_CRED_SOURCE_NVS ? "NVS" : "build-time");

        // During BLE provisioning, the prov manager internally connects WiFi
        // to verify credentials. Ignore that — don't start mDNS or notify.
        if (s_suppress_reconnect) {
            ESP_LOGI(TAG, "Got IP during provisioning — ignoring");
            return;
        }

        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        s_reconnecting = false;
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

    // Handle credential reset request (from boot button)
    if (s_reset_requested) {
        ESP_LOGW(TAG, "Credential reset requested — clearing NVS provisioned creds");
        wifi_prov_mgr_config_t prov_cfg = {
            .scheme = wifi_prov_scheme_ble,
            .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        };
        esp_err_t prov_ret = wifi_prov_mgr_init(prov_cfg);
        if (prov_ret == ESP_OK) {
            wifi_prov_mgr_reset_provisioning();
            wifi_prov_mgr_deinit();
        }
        s_reset_requested = false;
        s_has_nvs_creds = false;
        memset(s_nvs_ssid, 0, sizeof(s_nvs_ssid));
        memset(s_nvs_password, 0, sizeof(s_nvs_password));
        ESP_LOGI(TAG, "NVS credentials cleared — using build-time creds");
    } else {
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
            // Read NVS creds while storage is still FLASH (default).
            // Must do this before switching to RAM mode.
            s_suppress_reconnect = true;
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start());

            wifi_config_t stored_cfg = {0};
            if (esp_wifi_get_config(WIFI_IF_STA, &stored_cfg) == ESP_OK &&
                strlen((char *)stored_cfg.sta.ssid) > 0) {
                strncpy(s_nvs_ssid, (char *)stored_cfg.sta.ssid, sizeof(s_nvs_ssid) - 1);
                strncpy(s_nvs_password, (char *)stored_cfg.sta.password, sizeof(s_nvs_password) - 1);
                s_has_nvs_creds = true;
                ESP_LOGI(TAG, "Cached NVS credentials (SSID: '%s')", s_nvs_ssid);
            }

            esp_wifi_stop();
            s_suppress_reconnect = false;

            ESP_LOGI(TAG, "NVS and build-time credentials available — will scan and pick best");
        } else {
            ESP_LOGI(TAG, "Using compile-time WiFi credentials (no NVS creds)");
        }
    }

    // Switch to RAM-only storage so esp_wifi_set_config() doesn't overwrite NVS.
    // NVS credentials are managed exclusively by the BLE provisioning manager.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    // Set build-time creds as the initial config (scan_and_connect may override)
    use_buildtime_creds();

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

esp_err_t wifi_manager_suspend(void)
{
    ESP_LOGI(TAG, "Suspending WiFi (keeping driver running for provisioning)...");

    s_suppress_reconnect = true;
    s_reconnecting = false;
    xTimerStop(s_reconnect_timer, portMAX_DELAY);

    mdns_free();
    esp_wifi_disconnect();
    // NOTE: do NOT call esp_wifi_stop() — prov manager needs the driver

    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);

    ESP_LOGI(TAG, "WiFi suspended");
    return ESP_OK;
}

esp_err_t wifi_manager_stop(void)
{
    ESP_LOGI(TAG, "Stopping WiFi...");

    // Suppress reconnect attempts
    s_suppress_reconnect = true;
    s_reconnecting = false;
    xTimerStop(s_reconnect_timer, portMAX_DELAY);

    // Tear down mDNS
    mdns_free();

    // Disconnect and stop WiFi driver (keep initialized — prov manager needs it)
    esp_wifi_disconnect();
    esp_wifi_stop();

    xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);

    ESP_LOGI(TAG, "WiFi stopped");
    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    ESP_LOGI(TAG, "Starting WiFi...");

    // Suppress reconnect during entire restart sequence to prevent races
    // from stale disconnect events arriving asynchronously
    s_suppress_reconnect = true;
    s_reconnecting = false;
    s_scanning = false;
    s_reconnect_delay_ms = RECONNECT_BASE_MS;
    xTimerStop(s_reconnect_timer, portMAX_DELAY);

    // Clean stop of any existing WiFi state
    esp_wifi_disconnect();
    esp_wifi_stop();

    // Re-cache NVS creds after provisioning (new creds may have been stored)
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
        // Temporarily use FLASH storage to read NVS creds, then switch back to RAM
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());

        wifi_config_t stored_cfg = {0};
        if (esp_wifi_get_config(WIFI_IF_STA, &stored_cfg) == ESP_OK &&
            strlen((char *)stored_cfg.sta.ssid) > 0) {
            strncpy(s_nvs_ssid, (char *)stored_cfg.sta.ssid, sizeof(s_nvs_ssid) - 1);
            strncpy(s_nvs_password, (char *)stored_cfg.sta.password, sizeof(s_nvs_password) - 1);
            s_has_nvs_creds = true;
            ESP_LOGI(TAG, "Updated cached NVS credentials (SSID: '%s')", s_nvs_ssid);
        }

        esp_wifi_stop();
        ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    }

    // Let any pending disconnect/STA_START events from temp start/stop drain
    vTaskDelay(pdMS_TO_TICKS(500));

    // Now enable reconnect and do the final start
    s_suppress_reconnect = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
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

void wifi_manager_request_credential_reset(void)
{
    s_reset_requested = true;
}

esp_err_t wifi_manager_clear_credentials(void)
{
    ESP_LOGW(TAG, "Clearing NVS WiFi credentials");

    // Clear the "provisioned" flag in NVS (don't use esp_wifi_restore — too destructive)
    wifi_prov_mgr_config_t prov_cfg = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    esp_err_t ret = wifi_prov_mgr_init(prov_cfg);
    if (ret == ESP_OK) {
        wifi_prov_mgr_reset_provisioning();
        wifi_prov_mgr_deinit();
    }

    // Clear cached NVS creds
    s_has_nvs_creds = false;
    memset(s_nvs_ssid, 0, sizeof(s_nvs_ssid));
    memset(s_nvs_password, 0, sizeof(s_nvs_password));

    // Load build-time creds and reconnect immediately
    use_buildtime_creds();
    s_reconnect_delay_ms = RECONNECT_BASE_MS;
    s_reconnecting = false;
    esp_wifi_disconnect();
    // Disconnect event will trigger schedule_reconnect → scan_and_connect
    // which will find only build-time creds available

    ESP_LOGI(TAG, "NVS credentials cleared — reconnecting with build-time creds");
    return ESP_OK;
}

wifi_cred_source_t wifi_manager_get_cred_source(void)
{
    return s_active_source;
}
