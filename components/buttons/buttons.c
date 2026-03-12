#include "buttons.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "buttons";

#define POLL_INTERVAL_MS    10
#define DEBOUNCE_MS         50
#define HOLD_MS             800
#define TASK_STACK_SIZE     4096
#define TASK_PRIORITY       5

typedef enum {
    BTN_STATE_IDLE,
    BTN_STATE_DEBOUNCING,
    BTN_STATE_PRESSED,
    BTN_STATE_WAIT_RELEASE,  // after event fired, wait for clean release
} btn_state_t;

typedef struct {
    int gpio;
    button_id_t id;
    btn_state_t state;
    TickType_t state_enter_tick;
} btn_context_t;

static btn_context_t s_buttons[2];
static button_callback_t s_cb = NULL;
static void *s_cb_data = NULL;

static void fire_event(button_id_t btn, button_event_t evt)
{
    ESP_LOGI(TAG, "btn=%s evt=%s",
             btn == BTN_ID_SPEED ? "SPEED" : "DIRECTION",
             evt == BTN_EVT_PRESS ? "PRESS" : "HOLD");
    if (s_cb) {
        s_cb(btn, evt, s_cb_data);
    }
}

static void poll_button(btn_context_t *btn)
{
    bool pressed = (gpio_get_level(btn->gpio) == 0); // active low, pull-up
    TickType_t now = xTaskGetTickCount();
    uint32_t elapsed_ms = (now - btn->state_enter_tick) * portTICK_PERIOD_MS;

    switch (btn->state) {
    case BTN_STATE_IDLE:
        if (pressed) {
            btn->state = BTN_STATE_DEBOUNCING;
            btn->state_enter_tick = now;
        }
        break;

    case BTN_STATE_DEBOUNCING:
        if (!pressed) {
            btn->state = BTN_STATE_IDLE;
        } else if (elapsed_ms >= DEBOUNCE_MS) {
            btn->state = BTN_STATE_PRESSED;
            btn->state_enter_tick = now;
        }
        break;

    case BTN_STATE_PRESSED:
        if (!pressed) {
            fire_event(btn->id, BTN_EVT_PRESS);
            btn->state = BTN_STATE_WAIT_RELEASE;
            btn->state_enter_tick = now;
        } else if (elapsed_ms >= HOLD_MS) {
            fire_event(btn->id, BTN_EVT_HOLD);
            btn->state = BTN_STATE_WAIT_RELEASE;
            btn->state_enter_tick = now;
        }
        break;

    case BTN_STATE_WAIT_RELEASE:
        if (pressed) {
            // Still held or bounce — reset release timer
            btn->state_enter_tick = now;
        } else if (elapsed_ms >= DEBOUNCE_MS) {
            // Confirmed clean release
            btn->state = BTN_STATE_IDLE;
        }
        break;
    }
}

static void button_poll_task(void *arg)
{
    while (1) {
        poll_button(&s_buttons[0]);
        poll_button(&s_buttons[1]);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

esp_err_t buttons_init(const buttons_config_t *config)
{
    // Configure GPIOs as inputs with internal pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->speed_gpio) | (1ULL << config->direction_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));

    s_buttons[0] = (btn_context_t){
        .gpio = config->speed_gpio,
        .id = BTN_ID_SPEED,
        .state = BTN_STATE_IDLE,
        .state_enter_tick = 0,
    };
    s_buttons[1] = (btn_context_t){
        .gpio = config->direction_gpio,
        .id = BTN_ID_DIRECTION,
        .state = BTN_STATE_IDLE,
        .state_enter_tick = 0,
    };

    BaseType_t ret = xTaskCreate(button_poll_task, "btn_poll",
                                  TASK_STACK_SIZE, NULL, TASK_PRIORITY, NULL);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "initialized: speed_gpio=%d direction_gpio=%d",
             config->speed_gpio, config->direction_gpio);
    return ESP_OK;
}

esp_err_t buttons_register_callback(button_callback_t cb, void *user_data)
{
    s_cb = cb;
    s_cb_data = user_data;
    return ESP_OK;
}
