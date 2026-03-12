#include "bts7960.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>

static const char *TAG = "bts7960";

#define LEDC_TIMER_NUM          LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_DUTY_RES           LEDC_TIMER_8_BIT
#define LEDC_FREQUENCY          25000
#define RPWM_CHANNEL            LEDC_CHANNEL_0
#define LPWM_CHANNEL            LEDC_CHANNEL_1

#define RAMP_UP_MS_PER_PCT      20      // 2s full ramp 0->100
#define RAMP_DOWN_MS_PER_PCT    50      // 5s full ramp 100->0
#define DIRECTION_COAST_MS      2000    // 2s coast at zero during direction flip

static int s_r_en_gpio;
static int s_l_en_gpio;

static int8_t s_current_output = 0;    // actual position (-100..+100)
static int8_t s_target_output = 0;     // desired position
static bool s_crossing_zero = false;   // ramp involves direction change
static bool s_coast_pause = false;     // in coast pause at zero

static SemaphoreHandle_t s_mutex;
static esp_timer_handle_t s_ramp_timer;
static bool s_ramp_active = false;

static uint32_t pct_to_duty(int pct)
{
    if (pct <= 0) return 0;
    if (pct >= 100) return 255;
    return (uint32_t)(pct * 255 / 100);
}

static void apply_output(int8_t output)
{
    if (output > 0) {
        ledc_set_duty(LEDC_MODE, RPWM_CHANNEL, pct_to_duty(output));
        ledc_update_duty(LEDC_MODE, RPWM_CHANNEL);
        ledc_set_duty(LEDC_MODE, LPWM_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LPWM_CHANNEL);
    } else if (output < 0) {
        ledc_set_duty(LEDC_MODE, RPWM_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, RPWM_CHANNEL);
        ledc_set_duty(LEDC_MODE, LPWM_CHANNEL, pct_to_duty(-output));
        ledc_update_duty(LEDC_MODE, LPWM_CHANNEL);
    } else {
        ledc_set_duty(LEDC_MODE, RPWM_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, RPWM_CHANNEL);
        ledc_set_duty(LEDC_MODE, LPWM_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LPWM_CHANNEL);
    }
}

static void schedule_next_step(void)
{
    // Next step moves toward or away from zero?
    int8_t next;
    if (s_current_output < s_target_output) next = s_current_output + 1;
    else next = s_current_output - 1;

    uint32_t ms;
    if (abs(next) < abs(s_current_output)) {
        ms = RAMP_DOWN_MS_PER_PCT;  // moving toward zero = slower
    } else {
        ms = RAMP_UP_MS_PER_PCT;    // moving away from zero = faster
    }

    esp_timer_start_once(s_ramp_timer, (uint64_t)ms * 1000);
}

static void ramp_timer_cb(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // Coast pause just ended — re-enable driver and continue
    if (s_coast_pause) {
        s_coast_pause = false;
        gpio_set_level(s_r_en_gpio, 1);
        gpio_set_level(s_l_en_gpio, 1);
        ESP_LOGI(TAG, "coast done, resuming ramp to %d", s_target_output);

        if (s_current_output != s_target_output) {
            schedule_next_step();
        } else {
            s_ramp_active = false;
            ESP_LOGI(TAG, "ramp complete: %d", s_current_output);
        }
        xSemaphoreGive(s_mutex);
        return;
    }

    // Step toward target
    if (s_current_output < s_target_output) {
        s_current_output++;
    } else if (s_current_output > s_target_output) {
        s_current_output--;
    }

    apply_output(s_current_output);

    // Hit zero during direction change — coast to let motor spin down
    if (s_current_output == 0 && s_target_output != 0 && s_crossing_zero) {
        s_crossing_zero = false;
        s_coast_pause = true;
        gpio_set_level(s_r_en_gpio, 0);
        gpio_set_level(s_l_en_gpio, 0);
        ESP_LOGI(TAG, "direction change: coasting %dms", DIRECTION_COAST_MS);
        esp_timer_start_once(s_ramp_timer, (uint64_t)DIRECTION_COAST_MS * 1000);
        xSemaphoreGive(s_mutex);
        return;
    }

    // Done or schedule next step
    if (s_current_output == s_target_output) {
        s_ramp_active = false;
        ESP_LOGI(TAG, "ramp complete: %d", s_current_output);
    } else {
        schedule_next_step();
        ESP_LOGD(TAG, "ramp: %d -> %d", s_current_output, s_target_output);
    }

    xSemaphoreGive(s_mutex);
}

esp_err_t bts7960_init(const bts7960_config_t *config)
{
    s_r_en_gpio = config->r_en_gpio;
    s_l_en_gpio = config->l_en_gpio;

    // LEDC timer: 25kHz, 8-bit resolution
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num = LEDC_TIMER_NUM,
        .freq_hz = LEDC_FREQUENCY,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    // RPWM channel (forward/exhaust)
    ledc_channel_config_t rpwm_conf = {
        .speed_mode = LEDC_MODE,
        .channel = RPWM_CHANNEL,
        .timer_sel = LEDC_TIMER_NUM,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = config->rpwm_gpio,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&rpwm_conf));

    // LPWM channel (reverse/intake)
    ledc_channel_config_t lpwm_conf = {
        .speed_mode = LEDC_MODE,
        .channel = LPWM_CHANNEL,
        .timer_sel = LEDC_TIMER_NUM,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = config->lpwm_gpio,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&lpwm_conf));

    // EN pins: GPIO output, drive HIGH to enable driver
    gpio_config_t en_conf = {
        .pin_bit_mask = (1ULL << config->r_en_gpio) | (1ULL << config->l_en_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&en_conf));
    gpio_set_level(config->r_en_gpio, 1);
    gpio_set_level(config->l_en_gpio, 1);

    // Mutex for ramp state
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    // Ramp timer (one-shot, re-scheduled each step for variable rate)
    esp_timer_create_args_t timer_args = {
        .callback = ramp_timer_cb,
        .name = "bts_ramp",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_ramp_timer));

    ESP_LOGI(TAG, "initialized: RPWM=%d LPWM=%d R_EN=%d L_EN=%d R_IS=%d L_IS=%d",
             config->rpwm_gpio, config->lpwm_gpio,
             config->r_en_gpio, config->l_en_gpio,
             config->r_is_gpio, config->l_is_gpio);

    return ESP_OK;
}

esp_err_t bts7960_set_output(int8_t speed_percent)
{
    if (speed_percent > 100) speed_percent = 100;
    if (speed_percent < -100) speed_percent = -100;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_target_output = speed_percent;

    // Detect direction change (current and target have opposite signs)
    s_crossing_zero = (s_current_output > 0 && speed_percent < 0) ||
                      (s_current_output < 0 && speed_percent > 0);

    ESP_LOGI(TAG, "set_output: target=%d current=%d cross_zero=%d",
             s_target_output, s_current_output, s_crossing_zero);

    if (s_current_output != s_target_output && !s_ramp_active) {
        s_ramp_active = true;
        schedule_next_step();
    }

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t bts7960_brake(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_ramp_active) {
        esp_timer_stop(s_ramp_timer);
        s_ramp_active = false;
    }
    s_current_output = 0;
    s_target_output = 0;
    s_crossing_zero = false;
    s_coast_pause = false;
    apply_output(0);
    gpio_set_level(s_r_en_gpio, 1);
    gpio_set_level(s_l_en_gpio, 1);

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "brake");
    return ESP_OK;
}

esp_err_t bts7960_coast(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_ramp_active) {
        esp_timer_stop(s_ramp_timer);
        s_ramp_active = false;
    }
    s_current_output = 0;
    s_target_output = 0;
    s_crossing_zero = false;
    s_coast_pause = false;
    apply_output(0);
    gpio_set_level(s_r_en_gpio, 0);
    gpio_set_level(s_l_en_gpio, 0);

    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "coast");
    return ESP_OK;
}

float bts7960_read_current(int channel)
{
    // Stub -- ADC current sense in Phase 6
    return 0.0f;
}

int8_t bts7960_get_current_output(void)
{
    int8_t val;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    val = s_current_output;
    xSemaphoreGive(s_mutex);
    return val;
}
