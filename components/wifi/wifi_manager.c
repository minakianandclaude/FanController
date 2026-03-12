#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include <string.h>

static const char *TAG = "wifi";

#define RECONNECT_BASE_MS   1000
#define RECONNECT_MAX_MS    60000

static EventGroupHandle_t s_wifi_events;
static const int CONNECTED_BIT = BIT0;

static TimerHandle_t s_reconnect_timer;
static uint32_t s_reconnect_delay_ms = RECONNECT_BASE_MS;

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
    ESP_LOGI(TAG, "Reconnecting (backoff %lums)...", (unsigned long)s_reconnect_delay_ms);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
    xTimerStart(s_reconnect_timer, 0);

    // Exponential backoff
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
            ESP_LOGI(TAG, "Connecting to %s...", CONFIG_VANFAN_WIFI_SSID);
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Disconnected");
            xEventGroupClearBits(s_wifi_events, CONNECTED_BIT);
            schedule_reconnect();
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, CONNECTED_BIT);
        s_reconnect_delay_ms = RECONNECT_BASE_MS;
        start_mdns();
    }
}

esp_err_t wifi_manager_init(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        return ESP_ERR_NO_MEM;
    }

    s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(RECONNECT_BASE_MS),
                                      pdFALSE, NULL, reconnect_timer_cb);
    if (!s_reconnect_timer) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_VANFAN_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_VANFAN_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA initialized");
    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    if (!s_wifi_events) return false;
    return (xEventGroupGetBits(s_wifi_events) & CONNECTED_BIT) != 0;
}
