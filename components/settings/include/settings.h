#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int8_t last_speed;
    int8_t last_direction;
    bool boot_auto_start;
    char hostname[32];
} vanfan_settings_t;

esp_err_t settings_init(void);
esp_err_t settings_get(vanfan_settings_t *out);
esp_err_t settings_set_speed(int8_t speed);
esp_err_t settings_set_direction(int8_t direction);
esp_err_t settings_set_auto_start(bool enabled);
esp_err_t settings_set_hostname(const char *hostname);
