#pragma once

#include "esp_err.h"
#include "esp_http_server.h"
#include "fan_control.h"

esp_err_t event_emitter_init(void);
esp_err_t event_emitter_add_client(int fd, httpd_handle_t hd);
esp_err_t event_emitter_remove_client(int fd);
void event_emitter_notify(const fan_state_t *state, fan_command_source_t source);
esp_err_t event_emitter_stop(void);
esp_err_t event_emitter_start(void);
