#include "ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ota";

#define OTA_BUF_SIZE 1024

esp_err_t ota_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGE(TAG, "Failed to get running partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Running from partition: %s @ 0x%lx",
             running->label, (unsigned long)running->address);

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "Firmware pending verification — marking as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next) {
        ESP_LOGI(TAG, "Next update partition: %s @ 0x%lx",
                 next->label, (unsigned long)next->address);
    }

    return ESP_OK;
}

esp_err_t ota_handle_upload(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update started, content_len=%d", req->content_len);

    if (req->content_len <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "{\"error\":400,\"message\":\"No firmware data\"}");
        return ESP_OK;
    }

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA update partition found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"No update partition\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Writing to partition: %s @ 0x%lx (size: %lu)",
             update_partition->label, (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"OTA begin failed\"}");
        return ESP_OK;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"Out of memory\"}");
        return ESP_OK;
    }

    int remaining = req->content_len;
    int total_written = 0;
    bool failed = false;

    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);

        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Receive error at %d/%d bytes", total_written, req->content_len);
            failed = true;
            break;
        }

        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %d bytes: %s",
                     total_written, esp_err_to_name(err));
            failed = true;
            break;
        }

        total_written += received;
        remaining -= received;

        // Log progress every ~10%
        int pct = total_written * 100 / req->content_len;
        int prev_pct = (total_written - received) * 100 / req->content_len;
        if (pct / 10 != prev_pct / 10) {
            ESP_LOGI(TAG, "OTA progress: %d%% (%d/%d bytes)",
                     pct, total_written, req->content_len);
        }
    }

    free(buf);

    if (failed) {
        esp_ota_abort(ota_handle);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"OTA write failed\"}");
        return ESP_OK;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            httpd_resp_set_status(req, "422 Unprocessable Entity");
            httpd_resp_sendstr(req, "{\"error\":422,\"message\":\"Firmware image invalid\"}");
        } else {
            httpd_resp_set_status(req, "500 Internal Server Error");
            httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"OTA finalize failed\"}");
        }
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "{\"error\":500,\"message\":\"Failed to set boot partition\"}");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA success! %d bytes written to %s. Restarting...",
             total_written, update_partition->label);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"OTA update complete, restarting...\"}");

    // Brief delay to allow HTTP response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();

    return ESP_OK; // Never reached
}
