#include "status_led.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "status_led";

#define TASK_STACK_SIZE  4096
#define TASK_PRIORITY    2

static led_strip_handle_t s_strip;
static volatile status_led_state_t s_state = STATUS_LED_OFF;
static TaskHandle_t s_task_handle;

static void set_pixel(uint8_t r, uint8_t g, uint8_t b)
{
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

static void clear_pixel(void)
{
    led_strip_clear(s_strip);
}

static void led_task(void *arg)
{
    status_led_state_t current = STATUS_LED_OFF;
    int tick = 0;
    int success_ticks = 0;

    while (1) {
        status_led_state_t desired = s_state;

        // Reset tick counter on state change
        if (desired != current) {
            current = desired;
            tick = 0;
            success_ticks = 0;
            clear_pixel();
        }

        switch (current) {
        case STATUS_LED_OFF:
            clear_pixel();
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_WIFI_DISCONNECTED:
            // Red blink ~1Hz: 500ms on, 500ms off
            if (tick % 2 == 0) {
                set_pixel(32, 0, 0);  // dim red
            } else {
                clear_pixel();
            }
            tick++;
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_BLE_PROVISIONING:
            set_pixel(0, 0, 32);  // dim blue solid
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case STATUS_LED_SUCCESS:
            // Green for 2 seconds, then auto-off
            if (success_ticks < 4) {  // 4 x 500ms = 2s
                set_pixel(0, 32, 0);  // dim green
                success_ticks++;
            } else {
                s_state = STATUS_LED_OFF;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

esp_err_t status_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_VANFAN_PIN_STATUS_LED,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };

    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,  // 10MHz
        .flags.with_dma = false,
    };

    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED strip init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    clear_pixel();

    BaseType_t xret = xTaskCreate(led_task, "status_led",
                                   TASK_STACK_SIZE, NULL, TASK_PRIORITY, &s_task_handle);
    if (xret != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialized on GPIO %d", CONFIG_VANFAN_PIN_STATUS_LED);
    return ESP_OK;
}

void status_led_set_state(status_led_state_t state)
{
    s_state = state;
}
