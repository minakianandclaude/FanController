#include "fan_control.h"
#include "bts7960.h"
#include "buttons.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "fan_control";

#define CMD_QUEUE_DEPTH  16
#define TASK_STACK_SIZE  4096
#define TASK_PRIORITY    10

static fan_state_t s_state = {
    .running = false,
    .speed_percent = 20,
    .direction = FAN_DIR_EXHAUST,
    .mode = FAN_MODE_MANUAL,
};

static SemaphoreHandle_t s_state_mutex;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task_handle;

#define MAX_STATE_CBS 4
static fan_state_callback_t s_state_cbs[MAX_STATE_CBS];
static int s_num_state_cbs = 0;

static const char *cmd_name(fan_command_type_t type)
{
    switch (type) {
        case FAN_CMD_TURN_ON:          return "TURN_ON";
        case FAN_CMD_TURN_OFF:         return "TURN_OFF";
        case FAN_CMD_TOGGLE:           return "TOGGLE";
        case FAN_CMD_SET_SPEED:        return "SET_SPEED";
        case FAN_CMD_SPEED_CYCLE:      return "SPEED_CYCLE";
        case FAN_CMD_SET_DIRECTION:    return "SET_DIRECTION";
        case FAN_CMD_DIRECTION_TOGGLE: return "DIRECTION_TOGGLE";
        case FAN_CMD_SET_COMBINED:     return "SET_COMBINED";
        case FAN_CMD_EMERGENCY_STOP:   return "EMERGENCY_STOP";
        case FAN_CMD_SET_MODE:         return "SET_MODE";
        default:                       return "UNKNOWN";
    }
}

static const char *src_name(fan_command_source_t src)
{
    switch (src) {
        case FAN_SRC_BUTTON:  return "BUTTON";
        case FAN_SRC_API:     return "API";
        case FAN_SRC_STARTUP: return "STARTUP";
        default:              return "UNKNOWN";
    }
}

static const char *dir_name(fan_direction_t dir)
{
    return dir == FAN_DIR_EXHAUST ? "EXHAUST" : "INTAKE";
}

static void apply_state_to_driver(void)
{
    if (s_state.running) {
        int8_t output = (int8_t)(s_state.speed_percent * (int8_t)s_state.direction);
        bts7960_set_output(output);
    } else {
        bts7960_set_output(0);
    }
}

static void notify_state_change(fan_command_source_t source)
{
    for (int i = 0; i < s_num_state_cbs; i++) {
        if (s_state_cbs[i]) {
            s_state_cbs[i](&s_state, source);
        }
    }
}

static void log_state(fan_command_type_t cmd_type, fan_command_source_t source)
{
    ESP_LOGI(TAG, "cmd=%s src=%s | state: running=%d speed=%d dir=%s",
             cmd_name(cmd_type), src_name(source),
             s_state.running, s_state.speed_percent, dir_name(s_state.direction));
}

static void process_command(const fan_command_t *cmd)
{
    switch (cmd->type) {
    case FAN_CMD_TURN_ON:
        s_state.running = true;
        if (cmd->speed > 0) {
            s_state.speed_percent = cmd->speed;
        }
        if (cmd->direction == FAN_DIR_EXHAUST || cmd->direction == FAN_DIR_INTAKE) {
            s_state.direction = cmd->direction;
        }
        break;

    case FAN_CMD_TURN_OFF:
        s_state.running = false;
        break;

    case FAN_CMD_TOGGLE:
        s_state.running = !s_state.running;
        break;

    case FAN_CMD_SET_SPEED:
        if (cmd->speed >= 1 && cmd->speed <= 100) {
            s_state.speed_percent = cmd->speed;
            if (!s_state.running) {
                s_state.running = true;
            }
        }
        break;

    case FAN_CMD_SPEED_CYCLE:
        if (s_state.speed_percent < 40) s_state.speed_percent = 40;
        else if (s_state.speed_percent < 60) s_state.speed_percent = 60;
        else if (s_state.speed_percent < 80) s_state.speed_percent = 80;
        else if (s_state.speed_percent < 100) s_state.speed_percent = 100;
        else s_state.speed_percent = 20;
        break;

    case FAN_CMD_SET_DIRECTION:
        if (cmd->direction == FAN_DIR_EXHAUST || cmd->direction == FAN_DIR_INTAKE) {
            s_state.direction = cmd->direction;
        }
        break;

    case FAN_CMD_DIRECTION_TOGGLE:
        s_state.direction = (s_state.direction == FAN_DIR_EXHAUST)
                            ? FAN_DIR_INTAKE : FAN_DIR_EXHAUST;
        break;

    case FAN_CMD_SET_COMBINED:
        if (cmd->speed >= 1 && cmd->speed <= 100) {
            s_state.speed_percent = cmd->speed;
        }
        if (cmd->direction == FAN_DIR_EXHAUST || cmd->direction == FAN_DIR_INTAKE) {
            s_state.direction = cmd->direction;
        }
        if (!s_state.running) {
            s_state.running = true;
        }
        break;

    case FAN_CMD_EMERGENCY_STOP:
        s_state.running = false;
        bts7960_brake();
        break;

    case FAN_CMD_SET_MODE:
        s_state.mode = cmd->mode;
        break;
    }

    log_state(cmd->type, cmd->source);

    // Apply to motor (emergency stop already handled brake directly)
    if (cmd->type != FAN_CMD_EMERGENCY_STOP) {
        apply_state_to_driver();
    }

    notify_state_change(cmd->source);
}

static void button_callback(button_id_t btn, button_event_t evt, void *user_data)
{
    fan_command_t cmd = {
        .source = FAN_SRC_BUTTON,
        .speed = 0,
        .direction = 0,
        .mode = FAN_MODE_MANUAL,
    };

    // Read current state to decide command
    fan_state_t state;
    fan_control_get_state(&state);

    if (!state.running) {
        if (evt == BTN_EVT_PRESS) {
            // Either button press when off: turn on with last config
            cmd.type = FAN_CMD_TURN_ON;
        } else {
            // Hold when off: quick-start shortcuts
            cmd.type = FAN_CMD_TURN_ON;
            cmd.speed = 20;
            if (btn == BTN_ID_SPEED) {
                cmd.direction = FAN_DIR_INTAKE;
            } else {
                cmd.direction = FAN_DIR_EXHAUST;
            }
        }
    } else {
        if (evt == BTN_EVT_HOLD) {
            cmd.type = FAN_CMD_TURN_OFF;
        } else if (btn == BTN_ID_SPEED) {
            cmd.type = FAN_CMD_SPEED_CYCLE;
        } else {
            cmd.type = FAN_CMD_DIRECTION_TOGGLE;
        }
    }

    fan_control_send_command(&cmd);
}

static void fan_control_task(void *arg)
{
    fan_command_t cmd;
    while (1) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            process_command(&cmd);
            xSemaphoreGive(s_state_mutex);
        }
    }
}

esp_err_t fan_control_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    if (!s_state_mutex) {
        ESP_LOGE(TAG, "Failed to create state mutex");
        return ESP_ERR_NO_MEM;
    }

    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(fan_command_t));
    if (!s_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ret = xTaskCreate(fan_control_task, "fan_control",
                                 TASK_STACK_SIZE, NULL, TASK_PRIORITY, &s_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create fan_control task");
        return ESP_FAIL;
    }

    // Register ourselves as button callback handler
    buttons_register_callback(button_callback, NULL);

    ESP_LOGI(TAG, "Initialized: speed=%d dir=%s mode=MANUAL",
             s_state.speed_percent, dir_name(s_state.direction));

    return ESP_OK;
}

esp_err_t fan_control_send_command(const fan_command_t *cmd)
{
    if (!cmd || !s_cmd_queue) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xQueueSend(s_cmd_queue, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Command queue full, dropping cmd=%s", cmd_name(cmd->type));
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t fan_control_get_state(fan_state_t *out)
{
    if (!out || !s_state_mutex) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(out, &s_state, sizeof(fan_state_t));
    xSemaphoreGive(s_state_mutex);

    return ESP_OK;
}

esp_err_t fan_control_register_state_cb(fan_state_callback_t cb)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_num_state_cbs >= MAX_STATE_CBS) {
        ESP_LOGE(TAG, "Max state callbacks reached (%d)", MAX_STATE_CBS);
        return ESP_ERR_NO_MEM;
    }

    s_state_cbs[s_num_state_cbs++] = cb;
    return ESP_OK;
}

void fan_control_set_temp_input(float temp_c)
{
    // Phase 6 stub — auto mode temperature input
    (void)temp_c;
}
