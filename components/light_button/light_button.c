#include "light_button.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "light_btn";

#define POLL_INTERVAL_MS    10
#define DEBOUNCE_MS         50
#define HOLD_MS             800
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       5

typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_DEBOUNCING,
    BTN_STATE_PRESSED,
    BTN_STATE_WAIT_RELEASE,
} btn_state_t;

static int s_gpio;
static btn_state_t s_state = BTN_STATE_IDLE;
static TickType_t s_state_enter_tick = 0;
static light_button_callback_t s_cb = NULL;
static void *s_cb_data = NULL;

static void fire_event(light_button_event_t evt)
{
    ESP_LOGI(TAG, "evt=%s", evt == LIGHT_BTN_EVT_PRESS ? "PRESS" : "HOLD");
    if (s_cb) {
        s_cb(evt, s_cb_data);
    }
}

static void poll_button(void)
{
    bool pressed = (gpio_get_level(s_gpio) == 0);  // active low, pull-up
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (now - s_state_enter_tick) * portTICK_PERIOD_MS;

    switch (s_state) {
    case BTN_STATE_IDLE:
        if (pressed) {
            s_state = BTN_STATE_DEBOUNCING;
            s_state_enter_tick = now;
        }
        break;

    case BTN_STATE_DEBOUNCING:
        if (!pressed) {
            s_state = BTN_STATE_IDLE;
        } else if (elapsed_ms >= DEBOUNCE_MS) {
            s_state = BTN_STATE_PRESSED;
            s_state_enter_tick = now;
        }
        break;

    case BTN_STATE_PRESSED:
        if (!pressed) {
            fire_event(LIGHT_BTN_EVT_PRESS);
            s_state = BTN_STATE_WAIT_RELEASE;
            s_state_enter_tick = now;
        } else if (elapsed_ms >= HOLD_MS) {
            fire_event(LIGHT_BTN_EVT_HOLD);
            s_state = BTN_STATE_WAIT_RELEASE;
            s_state_enter_tick = now;
        }
        break;

    case BTN_STATE_WAIT_RELEASE:
        if (pressed) {
            s_state_enter_tick = now;  // still held or bounce — reset
        } else if (elapsed_ms >= DEBOUNCE_MS) {
            s_state = BTN_STATE_IDLE;
        }
        break;
    }
}

static void light_button_task(void *arg)
{
    while (1) {
        poll_button();
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t light_button_init(const light_button_config_t *config)
{
    s_gpio = config->gpio;

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    BaseType_t ret = xTaskCreate(light_button_task, "light_btn",
                                  TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized: gpio=%d", config->gpio);
    return ESP_OK;
}

esp_err_t light_button_register_callback(light_button_callback_t cb, void *user_data)
{
    s_cb = cb;
    s_cb_data = user_data;
    return ESP_OK;
}
