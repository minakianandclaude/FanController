#include "event_emitter.h"
#include "led_control.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "event_emitter";

#define MAX_SSE_CLIENTS     4
#define KEEPALIVE_INTERVAL  15000
#define SSE_BUF_SIZE        256

typedef struct {
    int fd;
    httpd_handle_t hd;
    bool active;
} sse_client_t;

static sse_client_t s_clients[MAX_SSE_CLIENTS];
static SemaphoreHandle_t s_mutex;
static TimerHandle_t s_keepalive_timer;

static const char *dir_str(fan_direction_t dir)
{
    return dir == FAN_DIR_EXHAUST ? "exhaust" : "intake";
}

static const char *src_str(fan_command_source_t src)
{
    switch (src) {
        case FAN_SRC_BUTTON:  return "button";
        case FAN_SRC_API:     return "api";
        case FAN_SRC_STARTUP: return "startup";
        default:              return "unknown";
    }
}

static const char *led_src_str(led_command_source_t src)
{
    switch (src) {
        case LED_SRC_BUTTON: return "button";
        case LED_SRC_API:    return "api";
        default:             return "unknown";
    }
}

static int format_state_event(char *buf, size_t len,
                               const fan_state_t *state,
                               fan_command_source_t source)
{
    return snprintf(buf, len,
        "data: {\"running\":%s,\"speed\":%d,\"direction\":\"%s\","
        "\"mode\":\"manual\",\"source\":\"%s\"}\n\n",
        state->running ? "true" : "false",
        state->speed_percent,
        dir_str(state->direction),
        src_str(source));
}

static int format_lights_event(char *buf, size_t len,
                                const led_state_t *state,
                                led_command_source_t source)
{
    return snprintf(buf, len,
        "event: lights\n"
        "data: {\"zones\":["
        "{\"on\":%s,\"brightness\":%d},"
        "{\"on\":%s,\"brightness\":%d},"
        "{\"on\":%s,\"brightness\":%d}],"
        "\"source\":\"%s\"}\n\n",
        state->zones[0].on ? "true" : "false", state->zones[0].brightness,
        state->zones[1].on ? "true" : "false", state->zones[1].brightness,
        state->zones[2].on ? "true" : "false", state->zones[2].brightness,
        led_src_str(source));
}

static void send_to_client(sse_client_t *client, const char *buf, size_t len)
{
    int ret = httpd_socket_send(client->hd, client->fd, buf, len, 0);
    if (ret < 0) {
        ESP_LOGW(TAG, "Send failed to fd %d, removing client", client->fd);
        httpd_sess_trigger_close(client->hd, client->fd);
        client->active = false;
    }
}

static void keepalive_timer_cb(TimerHandle_t timer)
{
    const char *comment = ": keepalive\n\n";
    size_t len = strlen(comment);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_clients[i].active) {
            send_to_client(&s_clients[i], comment, len);
        }
    }
    xSemaphoreGive(s_mutex);
}

static void state_change_cb(const fan_state_t *state, fan_command_source_t source)
{
    char buf[SSE_BUF_SIZE];
    int len = format_state_event(buf, sizeof(buf), state, source);
    if (len <= 0 || (size_t)len >= sizeof(buf)) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_clients[i].active) {
            send_to_client(&s_clients[i], buf, len);
        }
    }
    xSemaphoreGive(s_mutex);
}

static void lights_state_change_cb(const led_state_t *state, led_command_source_t source)
{
    char buf[SSE_BUF_SIZE];
    int len = format_lights_event(buf, sizeof(buf), state, source);
    if (len <= 0 || (size_t)len >= sizeof(buf)) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_clients[i].active) {
            send_to_client(&s_clients[i], buf, len);
        }
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t event_emitter_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_clients, 0, sizeof(s_clients));

    s_keepalive_timer = xTimerCreate("sse_ka", pdMS_TO_TICKS(KEEPALIVE_INTERVAL),
                                      pdTRUE, NULL, keepalive_timer_cb);
    if (!s_keepalive_timer) {
        return ESP_ERR_NO_MEM;
    }
    xTimerStart(s_keepalive_timer, 0);

    fan_control_register_state_cb(state_change_cb);
    led_control_register_state_cb(lights_state_change_cb);

    ESP_LOGI(TAG, "Initialized (max %d SSE clients)", MAX_SSE_CLIENTS);
    return ESP_OK;
}

esp_err_t event_emitter_add_client(int fd, httpd_handle_t hd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int slot = -1;
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (!s_clients[i].active) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(s_mutex);
        ESP_LOGW(TAG, "No free SSE client slots");
        return ESP_ERR_NO_MEM;
    }

    s_clients[slot].fd = fd;
    s_clients[slot].hd = hd;
    s_clients[slot].active = true;

    // Send initial fan state
    fan_state_t fan;
    fan_control_get_state(&fan);
    char buf[SSE_BUF_SIZE];
    int len = format_state_event(buf, sizeof(buf), &fan, FAN_SRC_API);
    if (len > 0 && (size_t)len < sizeof(buf)) {
        send_to_client(&s_clients[slot], buf, len);
    }

    // Send initial light state
    led_state_t lights;
    led_control_get_state(&lights);
    len = format_lights_event(buf, sizeof(buf), &lights, LED_SRC_API);
    if (len > 0 && (size_t)len < sizeof(buf)) {
        send_to_client(&s_clients[slot], buf, len);
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "SSE client added: fd=%d slot=%d", fd, slot);
    return ESP_OK;
}

esp_err_t event_emitter_remove_client(int fd)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            s_clients[i].active = false;
            ESP_LOGI(TAG, "SSE client removed: fd=%d slot=%d", fd, i);
            break;
        }
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t event_emitter_stop(void)
{
    // Stop keepalive timer to prevent sends on stale handles
    if (s_keepalive_timer) {
        xTimerStop(s_keepalive_timer, portMAX_DELAY);
    }

    // Clear all clients
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_SSE_CLIENTS; i++) {
        s_clients[i].active = false;
    }
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Stopped — all SSE clients cleared");
    return ESP_OK;
}

esp_err_t event_emitter_start(void)
{
    // Restart keepalive timer (client list is empty; new SSE connections will register)
    if (s_keepalive_timer) {
        xTimerStart(s_keepalive_timer, 0);
    }

    ESP_LOGI(TAG, "Started — ready for SSE clients");
    return ESP_OK;
}
