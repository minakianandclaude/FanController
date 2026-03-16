#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#define LED_ZONE_COUNT 3

typedef struct {
    bool on;
    uint8_t brightness;  // 0-100
} led_zone_state_t;

typedef struct {
    led_zone_state_t zones[LED_ZONE_COUNT];
} led_state_t;

typedef enum {
    LED_SRC_BUTTON,
    LED_SRC_API,
} led_command_source_t;

typedef void (*led_state_callback_t)(const led_state_t *state, led_command_source_t source);

typedef struct {
    int zone_gpios[LED_ZONE_COUNT];
} led_control_config_t;

// Button press action: 0-2 = toggle specific zone, -1 = toggle all
#define LED_BTN_ACTION_ALL (-1)

esp_err_t led_control_init(const led_control_config_t *config);
esp_err_t led_control_set_zone(int zone, bool on, uint8_t brightness, led_command_source_t src);
esp_err_t led_control_toggle_zone(int zone, led_command_source_t src);
esp_err_t led_control_toggle_all(led_command_source_t src);
esp_err_t led_control_set_all(bool on, uint8_t brightness, led_command_source_t src);
esp_err_t led_control_all_off(led_command_source_t src);
esp_err_t led_control_get_state(led_state_t *out);
esp_err_t led_control_register_state_cb(led_state_callback_t cb);

void led_control_set_button_action(int zone);
int led_control_get_button_action(void);
