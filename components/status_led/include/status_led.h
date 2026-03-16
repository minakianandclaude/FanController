#pragma once

#include "esp_err.h"

typedef enum {
    STATUS_LED_OFF,
    STATUS_LED_WIFI_DISCONNECTED,   // red slow blink ~1Hz
    STATUS_LED_BLE_PROVISIONING,    // blue solid
    STATUS_LED_SUCCESS,             // green 2s then off
    STATUS_LED_WIFI_RESET,          // rapid yellow blink 3s then off
    STATUS_LED_PROV_FAILED,         // rapid red blink 3s then back to blue
} status_led_state_t;

esp_err_t status_led_init(void);
void status_led_set_state(status_led_state_t state);
