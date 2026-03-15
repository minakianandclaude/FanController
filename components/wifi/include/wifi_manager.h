#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_state_callback_t)(bool connected);

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_stop(void);
esp_err_t wifi_manager_start(void);
bool wifi_manager_is_connected(void);
esp_err_t wifi_manager_register_state_cb(wifi_state_callback_t cb);
