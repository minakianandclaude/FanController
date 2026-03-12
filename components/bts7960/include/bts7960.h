#pragma once

#include "esp_err.h"

typedef struct {
    int rpwm_gpio;
    int lpwm_gpio;
    int r_en_gpio;
    int l_en_gpio;
    int r_is_gpio;
    int l_is_gpio;
} bts7960_config_t;

esp_err_t bts7960_init(const bts7960_config_t *config);
esp_err_t bts7960_set_output(int8_t speed_percent);
esp_err_t bts7960_brake(void);
esp_err_t bts7960_coast(void);
float bts7960_read_current(int channel);
int8_t bts7960_get_current_output(void);
