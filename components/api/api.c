#include "api.h"
#include "fan_control.h"
#include "led_control.h"
#include "event_emitter.h"
#include "ota.h"
#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>
#include <sys/time.h>

static const char *TAG = "api";

#define MAX_POST_BODY 512

static httpd_handle_t s_server;
static int64_t s_boot_time_us;

// ---------- helpers ----------

static const char *dir_str(fan_direction_t dir)
{
    return dir == FAN_DIR_EXHAUST ? "exhaust" : "intake";
}

static char *state_to_json(const fan_state_t *state)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", state->running);
    cJSON_AddNumberToObject(root, "speed", state->speed_percent);
    cJSON_AddStringToObject(root, "direction", dir_str(state->direction));
    cJSON_AddStringToObject(root, "mode", "manual");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t send_state_response(httpd_req_t *req)
{
    fan_state_t state;
    fan_control_get_state(&state);
    char *json = state_to_json(&state);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "error", code);
    cJSON_AddStringToObject(root, "message", message);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, code == 400 ? "400 Bad Request" :
                                code == 422 ? "422 Unprocessable Entity" :
                                "500 Internal Server Error");
    httpd_resp_sendstr(req, json ? json : "{\"error\":500}");
    free(json);
    return ESP_OK;
}

static cJSON *read_post_json(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > MAX_POST_BODY) {
        send_error(req, 400, "Invalid content length");
        return NULL;
    }

    char buf[MAX_POST_BODY + 1];
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        send_error(req, 400, "Failed to read body");
        return NULL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        send_error(req, 400, "Invalid JSON");
        return NULL;
    }
    return root;
}

// ---------- GET /api/v1/status ----------

static esp_err_t handle_status(httpd_req_t *req)
{
    return send_state_response(req);
}

// ---------- POST /api/v1/speed ----------

static esp_err_t handle_speed(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *speed_item = cJSON_GetObjectItem(root, "speed");
    if (!cJSON_IsNumber(speed_item)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'speed' field");
    }

    int speed = speed_item->valueint;
    cJSON_Delete(root);

    if (speed < 1 || speed > 100) {
        return send_error(req, 422, "Speed must be 1-100");
    }

    fan_command_t cmd = {
        .type = FAN_CMD_SET_SPEED,
        .source = FAN_SRC_API,
        .speed = (int8_t)speed,
    };
    fan_control_send_command(&cmd);

    // Brief delay for state machine to process
    vTaskDelay(pdMS_TO_TICKS(10));
    return send_state_response(req);
}

// ---------- POST /api/v1/direction ----------

static esp_err_t handle_direction(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *dir_item = cJSON_GetObjectItem(root, "direction");
    if (!cJSON_IsString(dir_item)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'direction' field");
    }

    fan_direction_t dir;
    if (strcmp(dir_item->valuestring, "exhaust") == 0) {
        dir = FAN_DIR_EXHAUST;
    } else if (strcmp(dir_item->valuestring, "intake") == 0) {
        dir = FAN_DIR_INTAKE;
    } else {
        cJSON_Delete(root);
        return send_error(req, 422, "Direction must be 'exhaust' or 'intake'");
    }
    cJSON_Delete(root);

    fan_command_t cmd = {
        .type = FAN_CMD_SET_DIRECTION,
        .source = FAN_SRC_API,
        .direction = dir,
    };
    fan_control_send_command(&cmd);

    vTaskDelay(pdMS_TO_TICKS(10));
    return send_state_response(req);
}

// ---------- POST /api/v1/mode ----------

static esp_err_t handle_mode(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *mode_item = cJSON_GetObjectItem(root, "mode");
    if (!cJSON_IsString(mode_item)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'mode' field");
    }

    if (strcmp(mode_item->valuestring, "manual") != 0) {
        cJSON_Delete(root);
        return send_error(req, 422, "Only 'manual' mode is supported");
    }
    cJSON_Delete(root);

    fan_command_t cmd = {
        .type = FAN_CMD_SET_MODE,
        .source = FAN_SRC_API,
        .mode = FAN_MODE_MANUAL,
    };
    fan_control_send_command(&cmd);

    vTaskDelay(pdMS_TO_TICKS(10));
    return send_state_response(req);
}

// ---------- POST /api/v1/set ----------

static esp_err_t handle_set(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    fan_command_t cmd = {
        .type = FAN_CMD_SET_COMBINED,
        .source = FAN_SRC_API,
        .speed = 0,
        .direction = 0,
        .mode = FAN_MODE_MANUAL,
    };

    cJSON *speed_item = cJSON_GetObjectItem(root, "speed");
    if (cJSON_IsNumber(speed_item)) {
        int speed = speed_item->valueint;
        if (speed < 1 || speed > 100) {
            cJSON_Delete(root);
            return send_error(req, 422, "Speed must be 1-100");
        }
        cmd.speed = (int8_t)speed;
    }

    cJSON *dir_item = cJSON_GetObjectItem(root, "direction");
    if (cJSON_IsString(dir_item)) {
        if (strcmp(dir_item->valuestring, "exhaust") == 0) {
            cmd.direction = FAN_DIR_EXHAUST;
        } else if (strcmp(dir_item->valuestring, "intake") == 0) {
            cmd.direction = FAN_DIR_INTAKE;
        } else {
            cJSON_Delete(root);
            return send_error(req, 422, "Direction must be 'exhaust' or 'intake'");
        }
    }

    cJSON_Delete(root);

    // Only send if at least one field was provided
    if (cmd.speed != 0 || cmd.direction != 0) {
        fan_control_send_command(&cmd);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return send_state_response(req);
}

// ---------- POST /api/v1/toggle ----------

static esp_err_t handle_toggle(httpd_req_t *req)
{
    fan_command_t cmd = {
        .type = FAN_CMD_TOGGLE,
        .source = FAN_SRC_API,
    };
    fan_control_send_command(&cmd);

    vTaskDelay(pdMS_TO_TICKS(10));
    return send_state_response(req);
}

// ---------- POST /api/v1/stop ----------

static esp_err_t handle_stop(httpd_req_t *req)
{
    fan_command_t cmd = {
        .type = FAN_CMD_EMERGENCY_STOP,
        .source = FAN_SRC_API,
    };
    fan_control_send_command(&cmd);

    vTaskDelay(pdMS_TO_TICKS(10));
    return send_state_response(req);
}

// ---------- GET /api/v1/events (SSE) ----------

static esp_err_t handle_events(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return ESP_FAIL;
    }

    // Send SSE headers manually
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n";

    int ret = httpd_socket_send(req->handle, fd, headers, strlen(headers), 0);
    if (ret < 0) {
        ESP_LOGE(TAG, "Failed to send SSE headers to fd %d", fd);
        return ESP_FAIL;
    }

    // Add to emitter — dead clients auto-removed on failed sends
    event_emitter_add_client(fd, req->handle);

    // Return ESP_OK but DON'T call httpd_resp_send — connection stays open
    return ESP_OK;
}

// ---------- POST /api/v1/ota/update ----------

static esp_err_t handle_ota_update(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update requested — stopping fan for safety");

    fan_command_t cmd = {
        .type = FAN_CMD_EMERGENCY_STOP,
        .source = FAN_SRC_API,
    };
    fan_control_send_command(&cmd);
    vTaskDelay(pdMS_TO_TICKS(100));

    return ota_handle_upload(req);
}

// ---------- lights helpers ----------

static char *lights_state_to_json(void)
{
    led_state_t state;
    led_control_get_state(&state);

    cJSON *root = cJSON_CreateObject();
    cJSON *zones = cJSON_CreateArray();
    for (int i = 0; i < LED_ZONE_COUNT; i++) {
        cJSON *z = cJSON_CreateObject();
        cJSON_AddBoolToObject(z, "on", state.zones[i].on);
        cJSON_AddNumberToObject(z, "brightness", state.zones[i].brightness);
        cJSON_AddItemToArray(zones, z);
    }
    cJSON_AddItemToObject(root, "zones", zones);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static esp_err_t send_lights_response(httpd_req_t *req)
{
    char *json = lights_state_to_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// ---------- GET /api/v1/lights ----------

static esp_err_t handle_lights_status(httpd_req_t *req)
{
    return send_lights_response(req);
}

// ---------- POST /api/v1/lights/zone ----------

static esp_err_t handle_lights_zone(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *zone_item = cJSON_GetObjectItem(root, "zone");
    if (!cJSON_IsNumber(zone_item)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'zone' field");
    }
    int zone = zone_item->valueint;
    if (zone < 1 || zone > LED_ZONE_COUNT) {
        cJSON_Delete(root);
        return send_error(req, 422, "Zone must be 1-3");
    }

    // Get current state for defaults
    led_state_t current;
    led_control_get_state(&current);
    int idx = zone - 1;

    cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    uint8_t brightness = current.zones[idx].brightness;
    if (cJSON_IsNumber(brightness_item)) {
        int b = brightness_item->valueint;
        if (b < 0 || b > 100) {
            cJSON_Delete(root);
            return send_error(req, 422, "Brightness must be 0-100");
        }
        brightness = (uint8_t)b;
    }

    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    bool on;
    if (cJSON_IsBool(on_item)) {
        on = cJSON_IsTrue(on_item);
    } else {
        // Default: on if brightness > 0
        on = (brightness > 0);
    }

    cJSON_Delete(root);

    // If no brightness specified and turning on with 0, use default
    if (on && brightness == 0) {
        brightness = 50;
    }

    led_control_set_zone(idx, on, brightness, LED_SRC_API);
    return send_lights_response(req);
}

// ---------- POST /api/v1/lights/zones ----------

static esp_err_t handle_lights_zones(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *zones_arr = cJSON_GetObjectItem(root, "zones");
    if (!cJSON_IsArray(zones_arr)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'zones' array");
    }

    int count = cJSON_GetArraySize(zones_arr);
    if (count < 1) {
        cJSON_Delete(root);
        return send_error(req, 400, "'zones' array must not be empty");
    }

    // Validate all entries first
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(zones_arr, i);
        cJSON *zone_item = cJSON_GetObjectItem(entry, "zone");
        if (!cJSON_IsNumber(zone_item) || zone_item->valueint < 1 || zone_item->valueint > LED_ZONE_COUNT) {
            cJSON_Delete(root);
            return send_error(req, 422, "Each entry must have 'zone' 1-3");
        }
        cJSON *brightness_item = cJSON_GetObjectItem(entry, "brightness");
        if (cJSON_IsNumber(brightness_item)) {
            int b = brightness_item->valueint;
            if (b < 0 || b > 100) {
                cJSON_Delete(root);
                return send_error(req, 422, "Brightness must be 0-100");
            }
        }
    }

    // Apply all entries
    led_state_t current;
    led_control_get_state(&current);

    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(zones_arr, i);
        int idx = cJSON_GetObjectItem(entry, "zone")->valueint - 1;

        cJSON *brightness_item = cJSON_GetObjectItem(entry, "brightness");
        uint8_t brightness = current.zones[idx].brightness;
        if (cJSON_IsNumber(brightness_item)) {
            brightness = (uint8_t)brightness_item->valueint;
        }

        cJSON *on_item = cJSON_GetObjectItem(entry, "on");
        bool on;
        if (cJSON_IsBool(on_item)) {
            on = cJSON_IsTrue(on_item);
        } else {
            on = (brightness > 0);
        }

        if (on && brightness == 0) {
            brightness = 50;
        }

        led_control_set_zone(idx, on, brightness, LED_SRC_API);
    }

    cJSON_Delete(root);
    return send_lights_response(req);
}

// ---------- POST /api/v1/lights/all ----------

static esp_err_t handle_lights_all(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *brightness_item = cJSON_GetObjectItem(root, "brightness");
    uint8_t brightness = 50;
    if (cJSON_IsNumber(brightness_item)) {
        int b = brightness_item->valueint;
        if (b < 0 || b > 100) {
            cJSON_Delete(root);
            return send_error(req, 422, "Brightness must be 0-100");
        }
        brightness = (uint8_t)b;
    }

    cJSON *on_item = cJSON_GetObjectItem(root, "on");
    bool on = true;
    if (cJSON_IsBool(on_item)) {
        on = cJSON_IsTrue(on_item);
    }

    cJSON_Delete(root);

    led_control_set_all(on, brightness, LED_SRC_API);
    return send_lights_response(req);
}

// ---------- POST /api/v1/lights/off ----------

static esp_err_t handle_lights_off(httpd_req_t *req)
{
    led_control_all_off(LED_SRC_API);
    return send_lights_response(req);
}

// ---------- GET /api/v1/info ----------

static esp_err_t handle_info(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();

    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t now_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;
    int uptime_s = (int)((now_us - s_boot_time_us) / 1000000);

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", app->version);
    cJSON_AddNumberToObject(root, "uptime_s", uptime_s);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    cJSON *chip_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(chip_obj, "model", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(chip_obj, "cores", chip.cores);
    cJSON_AddNumberToObject(chip_obj, "revision",
                            chip.revision / 100 + (chip.revision % 100) / 100.0);
    cJSON_AddItemToObject(root, "chip", chip_obj);

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(root, "ota_partition", running->label);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

// ---------- GET /api/v1/lights/button ----------

static esp_err_t handle_lights_button_get(httpd_req_t *req)
{
    int action = led_control_get_button_action();
    cJSON *root = cJSON_CreateObject();
    if (action == LED_BTN_ACTION_ALL) {
        cJSON_AddStringToObject(root, "action", "all");
    } else {
        cJSON_AddStringToObject(root, "action", "zone");
        cJSON_AddNumberToObject(root, "zone", action + 1);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

// ---------- POST /api/v1/lights/button ----------

static esp_err_t handle_lights_button_set(httpd_req_t *req)
{
    cJSON *root = read_post_json(req);
    if (!root) return ESP_OK;

    cJSON *action_item = cJSON_GetObjectItem(root, "action");
    if (!cJSON_IsString(action_item)) {
        cJSON_Delete(root);
        return send_error(req, 400, "Missing or invalid 'action' field");
    }

    if (strcmp(action_item->valuestring, "all") == 0) {
        led_control_set_button_action(LED_BTN_ACTION_ALL);
    } else if (strcmp(action_item->valuestring, "zone") == 0) {
        cJSON *zone_item = cJSON_GetObjectItem(root, "zone");
        if (!cJSON_IsNumber(zone_item) || zone_item->valueint < 1 || zone_item->valueint > LED_ZONE_COUNT) {
            cJSON_Delete(root);
            return send_error(req, 422, "Zone must be 1-3");
        }
        led_control_set_button_action(zone_item->valueint - 1);
    } else {
        cJSON_Delete(root);
        return send_error(req, 422, "Action must be 'all' or 'zone'");
    }

    cJSON_Delete(root);
    return handle_lights_button_get(req);
}

// ---------- GET /api/v1/wifi ----------

static esp_err_t handle_wifi_status(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "connected", wifi_manager_is_connected());

    wifi_cred_source_t src = wifi_manager_get_cred_source();
    cJSON_AddStringToObject(root, "credential_source",
                            src == WIFI_CRED_SOURCE_NVS ? "nvs" : "build-time");

    wifi_config_t cfg = {0};
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        cJSON_AddStringToObject(root, "ssid", (char *)cfg.sta.ssid);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);
    return ESP_OK;
}

// ---------- POST /api/v1/wifi/reset ----------

static esp_err_t handle_wifi_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "WiFi credential reset requested via API");

    esp_err_t ret = wifi_manager_clear_credentials();
    if (ret != ESP_OK) {
        return send_error(req, 500, "Failed to clear credentials");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "message", "NVS credentials cleared, reconnecting with build-time credentials");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "{}");
    free(json);

    // clear_credentials already disconnects and triggers reconnect with build-time creds
    return ESP_OK;
}

// ---------- server init ----------

static const httpd_uri_t s_uri_handlers[] = {
    { .uri = "/api/v1/status",     .method = HTTP_GET,  .handler = handle_status },
    { .uri = "/api/v1/speed",      .method = HTTP_POST, .handler = handle_speed },
    { .uri = "/api/v1/direction",  .method = HTTP_POST, .handler = handle_direction },
    { .uri = "/api/v1/mode",       .method = HTTP_POST, .handler = handle_mode },
    { .uri = "/api/v1/set",        .method = HTTP_POST, .handler = handle_set },
    { .uri = "/api/v1/toggle",     .method = HTTP_POST, .handler = handle_toggle },
    { .uri = "/api/v1/stop",       .method = HTTP_POST, .handler = handle_stop },
    { .uri = "/api/v1/events",     .method = HTTP_GET,  .handler = handle_events },
    { .uri = "/api/v1/ota/update", .method = HTTP_POST, .handler = handle_ota_update },
    { .uri = "/api/v1/info",       .method = HTTP_GET,  .handler = handle_info },
    { .uri = "/api/v1/lights",      .method = HTTP_GET,  .handler = handle_lights_status },
    { .uri = "/api/v1/lights/zone", .method = HTTP_POST, .handler = handle_lights_zone },
    { .uri = "/api/v1/lights/zones",.method = HTTP_POST, .handler = handle_lights_zones },
    { .uri = "/api/v1/lights/all",  .method = HTTP_POST, .handler = handle_lights_all },
    { .uri = "/api/v1/lights/off",    .method = HTTP_POST, .handler = handle_lights_off },
    { .uri = "/api/v1/lights/button", .method = HTTP_GET,  .handler = handle_lights_button_get },
    { .uri = "/api/v1/lights/button", .method = HTTP_POST, .handler = handle_lights_button_set },
    { .uri = "/api/v1/wifi",          .method = HTTP_GET,  .handler = handle_wifi_status },
    { .uri = "/api/v1/wifi/reset",  .method = HTTP_POST, .handler = handle_wifi_reset },
};

esp_err_t api_init(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    s_boot_time_us = (int64_t)now.tv_sec * 1000000 + now.tv_usec;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    int count = sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]);
    for (int i = 0; i < count; i++) {
        httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
    }

    ESP_LOGI(TAG, "HTTP server started (%d endpoints)", count);
    return ESP_OK;
}

esp_err_t api_stop(void)
{
    // Stop event emitter BEFORE httpd to prevent stale handle access
    event_emitter_stop();

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
    return ESP_OK;
}

esp_err_t api_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to restart HTTP server: %s", esp_err_to_name(ret));
        return ret;
    }

    int count = sizeof(s_uri_handlers) / sizeof(s_uri_handlers[0]);
    for (int i = 0; i < count; i++) {
        httpd_register_uri_handler(s_server, &s_uri_handlers[i]);
    }

    // Restart event emitter after httpd is ready
    event_emitter_start();

    ESP_LOGI(TAG, "HTTP server restarted (%d endpoints)", count);
    return ESP_OK;
}
