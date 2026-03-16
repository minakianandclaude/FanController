#pragma once

#include "esp_err.h"

typedef enum {
    LIGHT_BTN_EVT_PRESS,
    LIGHT_BTN_EVT_HOLD,
} light_button_event_t;

typedef void (*light_button_callback_t)(light_button_event_t evt, void *user_data);

typedef struct {
    int gpio;
} light_button_config_t;

esp_err_t light_button_init(const light_button_config_t *config);
esp_err_t light_button_register_callback(light_button_callback_t cb, void *user_data);
