// Copyright 2019 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <stdlib.h>
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_log.h"

const static char *TAG = "eth_fork_netif_glue";

typedef struct {
    esp_netif_driver_base_t base;
    esp_eth_handle_t eth_driver;
} esp_eth_netif_glue_t;

esp_err_t pkt_eth2wifi(esp_eth_handle_t eth_handle, uint8_t *buffer, uint32_t len, void *priv);

static esp_err_t esp_eth_post_attach(esp_netif_t *esp_netif, void *args)
{
    uint8_t eth_mac[6];
    esp_eth_netif_glue_t *glue = (esp_eth_netif_glue_t *)args;
    glue->base.netif = esp_netif;

    esp_eth_update_input_path(glue->eth_driver, pkt_eth2wifi, esp_netif);

    // set driver related config to esp-netif
    esp_netif_driver_ifconfig_t driver_ifconfig = {
            .handle =  glue->eth_driver,
            .transmit = esp_eth_transmit,
            .driver_free_rx_buffer = NULL
    };

    ESP_ERROR_CHECK(esp_netif_set_driver_config(esp_netif, &driver_ifconfig));
    esp_eth_ioctl(glue->eth_driver, ETH_CMD_G_MAC_ADDR, eth_mac);

    esp_netif_set_mac(esp_netif, eth_mac);
    ESP_LOGI(TAG, "ethernet attached to netif");

    return ESP_OK;
}

void *eth_fork_netif_glue(esp_eth_handle_t eth_hdl)
{
    esp_eth_netif_glue_t *glue = calloc(1, sizeof(esp_eth_netif_glue_t));
    if (!glue) {
        ESP_LOGE(TAG, "create netif glue failed");
        return NULL;
    }
    glue->eth_driver = eth_hdl;
    glue->base.post_attach = esp_eth_post_attach;
    esp_eth_increase_reference(eth_hdl);
    return &glue->base;
}

