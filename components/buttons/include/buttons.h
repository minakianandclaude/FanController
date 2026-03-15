#pragma once

#include "esp_err.h"

typedef enum {
    BTN_ID_SPEED,
    BTN_ID_DIRECTION,
    BTN_ID_BOTH,
} button_id_t;

typedef enum {
    BTN_EVT_PRESS,
    BTN_EVT_HOLD,
    BTN_EVT_HOLD_BOTH,
} button_event_t;

typedef void (*button_callback_t)(button_id_t btn, button_event_t evt, void *user_data);

typedef struct {
    int speed_gpio;
    int direction_gpio;
} buttons_config_t;

esp_err_t buttons_init(const buttons_config_t *config);
esp_err_t buttons_register_callback(button_callback_t cb, void *user_data);
