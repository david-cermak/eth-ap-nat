/* OTA example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_wifi.h"

//#define HASH_LEN 32

static const char *TAG = "simple_ota";

esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
        break;
    case HTTP_EVENT_REDIRECT:
        ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
        break;
    }
    return ESP_OK;
}

void simple_ota_example_task(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting OTA example task");
    esp_err_t ret = ESP_OK;
    esp_http_client_config_t config = {
        .url = CONFIG_EXAMPLE_FIRMWARE_UPGRADE_URL,
        .event_handler = http_event_handler,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
    };
    ESP_LOGI(TAG, "Attempting to download update from %s", config.url);
    esp_https_ota_handle_t https_ota_handle = NULL;
    ESP_GOTO_ON_ERROR(esp_https_ota_begin(&ota_config, &https_ota_handle), err, TAG, "Failed to start OTA");
    ESP_GOTO_ON_FALSE(https_ota_handle != NULL, ESP_FAIL, err, TAG, "Failed to create OTA handle");

    int count = 0;
    while ((ret = esp_https_ota_perform(https_ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        ESP_LOGI(TAG, "esp_https_ota_perform() %d", count++);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (count%50 == 0) {
            esp_wifi_disconnect();
        }
    }
    ESP_GOTO_ON_ERROR(ret, abort, TAG, "Error while running OTA");

    ESP_GOTO_ON_ERROR(esp_https_ota_finish(https_ota_handle), err, TAG, "Fail to finish OTA");
    ESP_LOGI(TAG, "OTA Succeed, Rebooting...");
    esp_restart();

abort:
    ESP_LOGE(TAG, "Aborting OTA");
    esp_https_ota_abort(https_ota_handle);
err:
    ESP_LOGE(TAG, "Firmware upgrade failed %d", ret);
    vTaskDelete(NULL);
}

esp_err_t wifi_connect(void);

void app_main(void)
{
    ESP_LOGI(TAG, "OTA example app_main start");
    // Initialize NVS.
    ESP_ERROR_CHECK(wifi_connect());
    xTaskCreate(&simple_ota_example_task, "ota_example_task", 8192, NULL, 5, NULL);
}
