#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t ota_init(void);
esp_err_t ota_handle_upload(httpd_req_t *req);
