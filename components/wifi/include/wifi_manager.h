#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    WIFI_CRED_SOURCE_BUILD_TIME,
    WIFI_CRED_SOURCE_NVS,
} wifi_cred_source_t;

typedef void (*wifi_state_callback_t)(bool connected);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_stop(void);
esp_err_t wifi_manager_start(void);
esp_err_t wifi_manager_suspend(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_register_state_cb(wifi_state_callback_t cb);

void wifi_manager_request_credential_reset(void);
esp_err_t wifi_manager_clear_credentials(void);
wifi_cred_source_t wifi_manager_get_cred_source(void);
