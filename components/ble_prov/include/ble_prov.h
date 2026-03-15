#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*ble_prov_cb_t)(void);

esp_err_t ble_prov_init(ble_prov_cb_t on_creds_received);
esp_err_t ble_prov_start(void);
esp_err_t ble_prov_stop(void);
bool ble_prov_is_active(void);
