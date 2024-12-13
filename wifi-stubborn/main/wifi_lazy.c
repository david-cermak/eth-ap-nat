/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "esp_private/wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_wifi.h"
#include "esp_mac.h"
//
//  This is a simplified wifi-netif driver, which disconnects network in a lazy way,
//  so it doesn't eagerly close network interface on Wi-Fi disconnection,
//  it delays the decision expecting the Wi-Fi would reconnect
//

static const char *TAG = "lazy_wifi";

// Define a dummy driver for lazy-wifi (everything is static, h != null)
#define CUSTOM_WIFI_DRIVER (1)

static esp_netif_t *s_netif = NULL;
static bool s_wifi_connected = false;
static bool s_netif_connected = false;
static TimerHandle_t s_disconnect_timer = NULL;

static void wifi_free(void *h, void* buffer)
{
    if (buffer) {
        esp_wifi_internal_free_rx_buffer(buffer);
    }
}

static esp_err_t wifi_receive(void *buffer, uint16_t len, void *eb)
{
    if (s_netif == NULL) {
        return ESP_FAIL;
    }
    return esp_netif_receive(s_netif, buffer, len, eb);
}

static void wifi_start(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_t *netif = arg;
    uint8_t mac[6];
    __unused esp_err_t ret;
    ESP_GOTO_ON_ERROR(esp_wifi_get_mac(WIFI_IF_STA, mac), err, TAG, "Failed to get WiFi MAC");
    ESP_LOGI(TAG, "WIFI mac address: %x %x %x %x %x %x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_set_mac(netif, mac);
    esp_netif_action_start(netif, base, id, data);
    return;
err:
    ESP_LOGE(TAG, "Failed to start WiFi interface");
}

static void wifi_stop(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_netif_t *netif = arg;
    esp_netif_action_start(netif, base, id, data);
}

static void wifi_connected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    __unused esp_err_t ret;
    esp_netif_t *netif = arg;
    if (!s_wifi_connected) {
        ESP_GOTO_ON_ERROR(esp_wifi_internal_reg_rxcb(WIFI_IF_STA,  wifi_receive), err, TAG, "Failed to register WiFi cb");
        s_wifi_connected = true;
    }
    if (!s_netif_connected) {
        esp_netif_action_connected(netif, base, id, data);
    }
    // to cancel pending netif disconnection
    s_netif_connected = true;
    if (xTimerIsTimerActive(s_disconnect_timer)) {
        ESP_LOGW(TAG, "Canceling NETIF disconnect request");
        xTimerStop(s_disconnect_timer, 0);
    }
    return;
err:
    ESP_LOGE(TAG, "Failed to set WiFi interface to connected state");
}

static void wifi_disconnected(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    __unused esp_err_t ret;
    if (s_wifi_connected == true) {
        ESP_GOTO_ON_ERROR(esp_wifi_internal_reg_rxcb(WIFI_IF_STA,  NULL), err, TAG, "Failed to register WiFi cb");
        s_wifi_connected = false;
        if (s_disconnect_timer) {
            ESP_LOGW(TAG, "Will disconnect NETIF after 10 seconds");
            xTimerStart(s_disconnect_timer, 0);
        }
    }
    return;
err:
    ESP_LOGE(TAG, "Failed to set WiFi interface to disconnected state");
}

static void netif_disconnected(TimerHandle_t xTimer)
{
    ESP_LOGW(TAG, "NETIF has been disconnected");
    if (s_netif_connected == true) {
        esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
        s_netif_connected = false;
        xTimerStop(s_disconnect_timer, 0);
    }
}

static void wifi_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    __unused esp_err_t ret;
    esp_netif_t *netif = arg;
    ESP_GOTO_ON_ERROR(esp_wifi_internal_set_sta_ip(), err, TAG, "Failed to notify wifi about an IP");
    esp_netif_action_got_ip(netif, base, id, data);
    return;
err:
    ESP_LOGE(TAG, "Failed to set WiFi interface to connected state");
}

static void remove_wifi_handlers(void)
{
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_start);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_STOP, wifi_stop);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_connected);
    esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_disconnected);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_got_ip);
}

static esp_err_t set_wifi_handlers(esp_netif_t *netif)
{
    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, wifi_start, netif), err, TAG, "Failed to register wifi-start event");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, wifi_stop, netif), err, TAG, "Failed to register wifi-stop event");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, wifi_connected, netif), err, TAG, "Failed to register connected event");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_disconnected, netif), err, TAG, "Failed to register disconnected event");
    ESP_GOTO_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_got_ip, netif), err, TAG, "Failed to register got IP event");
    return ESP_OK;
err:
    remove_wifi_handlers();
    return ret;
}

static esp_err_t wifi_transmit(void *h, void *buffer, size_t len)
{
    if (s_wifi_connected) {
        return esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
    }
    return ESP_OK;
}

static esp_err_t wifi_transmit_wrap(void *h, void *buffer, size_t len, void *nb)
{
    return wifi_transmit(h, buffer, len);
}

esp_netif_t *create_lazy_wifi_sta(void)
{
    __unused esp_err_t ret;
    esp_netif_inherent_config_t inherent_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    inherent_cfg.if_key = "WIFI_LAZY";
    inherent_cfg.if_desc = "lazy wifi station";
    esp_netif_driver_ifconfig_t driver_cfg = {
            .handle = (esp_netif_iodriver_handle)CUSTOM_WIFI_DRIVER,
            .transmit = wifi_transmit,
            .transmit_wrap = wifi_transmit_wrap,
            .driver_free_rx_buffer = wifi_free
    };
    esp_netif_config_t cfg = {
            .base = &inherent_cfg,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA,
            .driver = &driver_cfg
    };

    s_netif = esp_netif_new(&cfg);
    ESP_RETURN_ON_FALSE(s_netif, NULL, TAG, "Failed to create netif");
    ESP_GOTO_ON_ERROR(set_wifi_handlers(s_netif), err, TAG, "Failed to register wifi handlers");
    s_disconnect_timer = xTimerCreate("disconnect_timer", pdMS_TO_TICKS(10000), pdTRUE, s_netif, netif_disconnected);
    ESP_GOTO_ON_FALSE(s_disconnect_timer, ESP_FAIL, err, TAG, "Failed to create timer");
    xTimerStop(s_disconnect_timer, 0);
    return s_netif;
err:
    esp_netif_destroy(s_netif);
    s_netif = NULL;
    return NULL;
}

