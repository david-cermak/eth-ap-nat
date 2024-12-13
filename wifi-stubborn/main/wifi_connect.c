/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_mac.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "wifi_connect";
static EventGroupHandle_t s_wifi_events;
static int s_retry_num = 0;
static const int s_max_retry = 20;

esp_netif_t *create_lazy_wifi_sta(void);

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < s_max_retry) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_connect(void)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_FALSE(s_wifi_events = xEventGroupCreate(), ESP_ERR_NO_MEM, err, TAG, "Failed to create wifi_events");
    ESP_GOTO_ON_ERROR(nvs_flash_init(), err, TAG, "Failed to init nvs flash");
    ESP_GOTO_ON_ERROR(esp_netif_init(), err, TAG, "Failed to init esp_netif");
    ESP_GOTO_ON_ERROR(esp_event_loop_create_default(), err, TAG, "Failed to create default event loop");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL),
                      err, TAG, "Failed to register WiFi event handler");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL),
                      err, TAG, "Failed to register IP event handler");

    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_GOTO_ON_ERROR(esp_wifi_init(&cfg), err, TAG, "Failed to initialize WiFi");
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), err, TAG, "Failed to set STA+AP mode");

    // Initialize STA
//    esp_netif_t *sta = esp_netif_create_default_wifi_sta();   // Default
    esp_netif_t *sta = create_lazy_wifi_sta();     // Custom (lazy-wifi)
    ESP_GOTO_ON_FALSE(sta, ESP_FAIL, err, TAG, "Failed to create WiFi station network interface");
    wifi_config_t wifi_sta_config = {
            .sta = {
                    .ssid = CONFIG_EXAMPLE_STA_SSID,
                    .password = CONFIG_EXAMPLE_STA_PASSWORD,
            },
    };
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config), err, TAG, "Failed to set STA config");

    // Start WiFi
    ESP_GOTO_ON_ERROR(esp_wifi_start(), err, TAG, "Failed to start WiFi");

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    ESP_GOTO_ON_FALSE((bits & WIFI_CONNECTED_BIT) == WIFI_CONNECTED_BIT, ESP_FAIL, err,
                      TAG, "Failed to obtain IP address from WiFi station");
    return ESP_OK;
err:
    esp_wifi_stop();
    esp_wifi_deinit();
    nvs_flash_deinit();
    esp_netif_deinit();
    esp_event_loop_delete_default();
    return ret;

}
