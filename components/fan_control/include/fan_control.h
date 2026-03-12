#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FAN_DIR_EXHAUST = 1,
    FAN_DIR_INTAKE = -1,
} fan_direction_t;

typedef enum {
    FAN_MODE_MANUAL,
} fan_mode_t;

typedef struct {
    bool running;
    int8_t speed_percent;
    fan_direction_t direction;
    fan_mode_t mode;
} fan_state_t;

typedef enum {
    FAN_CMD_TURN_ON,
    FAN_CMD_TURN_OFF,
    FAN_CMD_TOGGLE,
    FAN_CMD_SET_SPEED,
    FAN_CMD_SPEED_CYCLE,
    FAN_CMD_SET_DIRECTION,
    FAN_CMD_DIRECTION_TOGGLE,
    FAN_CMD_SET_COMBINED,
    FAN_CMD_EMERGENCY_STOP,
    FAN_CMD_SET_MODE,
} fan_command_type_t;

typedef enum {
    FAN_SRC_BUTTON,
    FAN_SRC_API,
    FAN_SRC_STARTUP,
} fan_command_source_t;

typedef struct {
    fan_command_type_t type;
    fan_command_source_t source;
    int8_t speed;
    fan_direction_t direction;
    fan_mode_t mode;
} fan_command_t;

typedef void (*fan_state_callback_t)(const fan_state_t *state, fan_command_source_t source);

esp_err_t fan_control_init(void);
esp_err_t fan_control_send_command(const fan_command_t *cmd);
esp_err_t fan_control_get_state(fan_state_t *out);
esp_err_t fan_control_register_state_cb(fan_state_callback_t cb);
void fan_control_set_temp_input(float temp_c);
