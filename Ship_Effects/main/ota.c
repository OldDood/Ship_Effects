#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include <sys/param.h>

// Reference your existing audio control to stop playback during flash
extern void play_stop(void);

static const char *TAG = "SHIP_OTA";

esp_err_t ship_ota_update_handler(httpd_req_t *req) {
    esp_ota_handle_t update_handle = 0;
    const esp_partition_t *update_partition = NULL;

    ESP_LOGI(TAG, "Starting OTA Update for San Juan...");

    // 1. Identify the inactive partition to write to
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Passive OTA partition not found");
        return ESP_FAIL;
    }

    // 2. Stop Core 1 Audio to prevent SPIFFS/Flash contention
    play_stop();
    ESP_LOGI(TAG, "Audio engine suspended for flashing");

    // 3. Begin OTA session
    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }

    // 4. Stream data from the web request
    char ota_buff[1024];
    int content_len = req->content_len;
    int remaining = content_len;
    int recv_len;

    while (remaining > 0) {
        if ((recv_len = httpd_req_recv(req, ota_buff, MIN(remaining, sizeof(ota_buff)))) <= 0) {
            if (recv_len == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "Timeout or error receiving web data");
            return ESP_FAIL;
        }

        err = esp_ota_write(update_handle, (const void *)ota_buff, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash write failed (%s)", esp_err_to_name(err));
            return ESP_FAIL;
        }
        remaining -= recv_len;
    }

    // 5. Finalize and swap boot partition
    if (esp_ota_end(update_handle) != ESP_OK || esp_ota_set_boot_partition(update_partition) != ESP_OK) {
        ESP_LOGE(TAG, "OTA finalization failed!");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Update Complete. Rebooting to %s...", update_partition->label);
    httpd_resp_sendstr(req, "Ship Firmware Updated Successfully. Rebooting...");
    
    // Short delay to allow the response to reach the browser
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}