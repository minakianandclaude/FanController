#include "led_control.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "led_control";

#define LEDC_TIMER_NUM   LEDC_TIMER_1
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES    LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY   1000

// Channels 2/3/4 — motor uses 0/1
static const ledc_channel_t s_channels[LED_ZONE_COUNT] = {
    LEDC_CHANNEL_2, LEDC_CHANNEL_3, LEDC_CHANNEL_4,
};

#define MAX_STATE_CBS 4

static led_state_t s_state;
static SemaphoreHandle_t s_mutex;
static led_state_callback_t s_state_cbs[MAX_STATE_CBS];
static int s_num_state_cbs = 0;
static int s_button_action = 0;  // default: toggle zone 0

#define DEFAULT_BRIGHTNESS 50

static uint32_t pct_to_duty(int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return 255;
    return (uint32_t)(pct * 255 / 100);
}

static void apply_zone(int zone)
{
    uint32_t duty = s_state.zones[zone].on
        ? pct_to_duty(s_state.zones[zone].brightness)
        : 0;
    ledc_set_duty(LEDC_MODE, s_channels[zone], duty);
    ledc_update_duty(LEDC_MODE, s_channels[zone]);
}

static void notify_state_change(led_command_source_t source)
{
    for (int i = 0; i < s_num_state_cbs; i++) {
        if (s_state_cbs[i]) {
            s_state_cbs[i](&s_state, source);
        }
    }
}

esp_err_t led_control_init(const led_control_config_t *config)
{
    // LEDC timer: 1kHz, 8-bit
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_NUM,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    // Configure a channel per zone
    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        ledc_channel_config_t ch_conf = {
            .speed_mode = LEDC_MODE,
            .channel = s_channels[i],
            .timer_sel = LEDC_TIMER_NUM,
            .intr_type = LEDC_INTR_DISABLE,
            .gpio_num = config->zone_gpios[i],
            .duty = 0,
            .hpoint = 0,
        };
        ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Initialize state: all off, default brightness remembered
    memset(&s_state, 0, sizeof(s_state));
    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        s_state.zones[i].on = false;
        s_state.zones[i].brightness = DEFAULT_BRIGHTNESS;
    }

    ESP_LOGI(TAG, "initialized: zones=%d GPIOs=[%d,%d,%d]",
             LED_ZONE_COUNT,
             config->zone_gpios[0], config->zone_gpios[1], config->zone_gpios[2]);
    return ESP_OK;
}

esp_err_t led_control_set_zone(int zone, bool on, uint8_t brightness, led_command_source_t src)
{
    if (zone < 0 || zone >= LED_ZONE_COUNT) return ESP_ERR_INVALID_ARG;
    if (brightness > 100) brightness = 100;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_state.zones[zone].on = on;
    s_state.zones[zone].brightness = brightness;
    apply_zone(zone);
    notify_state_change(src);

    ESP_LOGI(TAG, "zone %d: on=%d brightness=%d src=%s",
             zone + 1, on, brightness, src == LED_SRC_BUTTON ? "button" : "api");

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_toggle_zone(int zone, led_command_source_t src)
{
    if (zone < 0 || zone >= LED_ZONE_COUNT) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool new_on = !s_state.zones[zone].on;
    s_state.zones[zone].on = new_on;
    // If toggling on with brightness 0, restore default
    if (new_on && s_state.zones[zone].brightness == 0) {
        s_state.zones[zone].brightness = DEFAULT_BRIGHTNESS;
    }
    apply_zone(zone);
    notify_state_change(src);

    ESP_LOGI(TAG, "zone %d toggled: on=%d brightness=%d",
             zone + 1, new_on, s_state.zones[zone].brightness);

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_set_all(bool on, uint8_t brightness, led_command_source_t src)
{
    if (brightness > 100) brightness = 100;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        s_state.zones[i].on = on;
        s_state.zones[i].brightness = brightness;
        apply_zone(i);
    }
    notify_state_change(src);

    ESP_LOGI(TAG, "all zones: on=%d brightness=%d src=%s",
             on, brightness, src == LED_SRC_BUTTON ? "button" : "api");

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_toggle_all(led_command_source_t src)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // If any zone is on, turn all off. Otherwise turn all on.
    bool any_on = false;
    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        if (s_state.zones[i].on) { any_on = true; break; }
    }

    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        s_state.zones[i].on = !any_on;
        if (!any_on && s_state.zones[i].brightness == 0) {
            s_state.zones[i].brightness = DEFAULT_BRIGHTNESS;
        }
        apply_zone(i);
    }
    notify_state_change(src);

    ESP_LOGI(TAG, "all zones toggled: %s", any_on ? "off" : "on");

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_all_off(led_command_source_t src)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        s_state.zones[i].on = false;
        apply_zone(i);
    }
    notify_state_change(src);

    ESP_LOGI(TAG, "all zones off");

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_get_state(led_state_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t led_control_register_state_cb(led_state_callback_t cb)
{
    if (!cb) return ESP_ERR_INVALID_ARG;
    if (s_num_state_cbs >= MAX_STATE_CBS) {
        ESP_LOGE(TAG, "Max state callbacks reached (%d)", MAX_STATE_CBS);
        return ESP_ERR_NO_MEM;
    }
    s_state_cbs[s_num_state_cbs++] = cb;
    return ESP_OK;
}

void led_control_set_button_action(int zone)
{
    if (zone < -1 || zone >= LED_ZONE_COUNT) zone = 0;
    s_button_action = zone;
    ESP_LOGI(TAG, "button action set to: %s",
             zone == LED_BTN_ACTION_ALL ? "all" :
             zone == 0 ? "zone 1" : zone == 1 ? "zone 2" : "zone 3");
}

int led_control_get_button_action(void)
{
    return s_button_action;
}
