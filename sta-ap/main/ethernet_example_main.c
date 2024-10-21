/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_private/wifi.h"
#include "esp_mac.h"

static const char *TAG = "sta2ap_example";
static bool s_sta_is_connected = false;
static bool s_ethernet_is_connected = false;
static uint8_t s_sta_mac[6];
static uint8_t s_eth_mac[6];

typedef enum {
    FROM_WIRED,
    TO_WIRED
} mac_spoof_direction_t;

static void mac_spoof(mac_spoof_direction_t direction, uint8_t *buffer, uint16_t len, uint8_t own_mac[6]);


// Forward packets from Wi-Fi to Ethernet
static esp_err_t pkt_ap2sta(void *buffer, uint16_t len, void *eb)
{
    if (s_ethernet_is_connected) {
        mac_spoof(FROM_WIRED, buffer, len, s_sta_mac);
        esp_wifi_internal_tx(WIFI_IF_STA, buffer, len);
//        if (esp_eth_transmit(s_eth_handle, buffer, len) != ESP_OK) {
//            ESP_LOGE(TAG, "Ethernet send packet failed");
//        }
    }
    esp_wifi_internal_free_rx_buffer(eb);
    return ESP_OK;
}

// Forward packets from Ethernet to Wi-Fi
// Note that, Ethernet works faster than Wi-Fi on ESP32,
// so we need to add an extra queue to balance their speed difference.
static esp_err_t pkt_sta2ap(void *buffer, uint16_t len, void *eb)
{
    esp_err_t ret = ESP_OK;
    if (s_sta_is_connected) {
        mac_spoof(TO_WIRED, buffer, len, s_sta_mac);
        esp_wifi_internal_tx(WIFI_IF_AP, buffer, len);
    }
    esp_wifi_internal_free_rx_buffer(eb);
//
//    flow_control_msg_t msg = {
//        .packet = buffer,
//        .length = len
//    };
//    if (xQueueSend(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) != pdTRUE) {
//        ESP_LOGE(TAG, "send flow control message failed or timeout");
//        free(buffer);
//        ret = ESP_FAIL;
//    }
    return ret;
}

// This task will fetch the packet from the queue, and then send out through Wi-Fi.
// Wi-Fi handles packets slower than Ethernet, we might add some delay between each transmitting.
//static void eth2wifi_flow_control_task(void *args)
//{
//    flow_control_msg_t msg;
//    int res = 0;
//    uint32_t timeout = 0;
//    while (1) {
//        if (xQueueReceive(flow_control_queue, &msg, pdMS_TO_TICKS(FLOW_CONTROL_QUEUE_TIMEOUT_MS)) == pdTRUE) {
//            timeout = 0;
//            if (s_sta_is_connected && msg.length) {
//                do {
//                    vTaskDelay(pdMS_TO_TICKS(timeout));
//                    timeout += 2;
//                    res = esp_wifi_internal_tx(WIFI_IF_AP, msg.packet, msg.length);
//                } while (res && timeout < FLOW_CONTROL_WIFI_SEND_TIMEOUT_MS);
//                if (res != ESP_OK) {
//                    ESP_LOGE(TAG, "WiFi send packet failed: %d", res);
//                }
//            }
//            free(msg.packet);
//        }
//    }
//    vTaskDelete(NULL);
//}

// Event handler for Ethernet
//static void eth_event_handler(void *arg, esp_event_base_t event_base,
//                              int32_t event_id, void *event_data)
//{
//    switch (event_id) {
//    case ETHERNET_EVENT_CONNECTED:
//        ESP_LOGI(TAG, "Ethernet Link Up");
//        s_ethernet_is_connected = true;
//        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, s_eth_mac);
//        esp_wifi_set_mac(WIFI_IF_AP, s_eth_mac);
//        ESP_ERROR_CHECK(esp_wifi_start());
//        break;
//    case ETHERNET_EVENT_DISCONNECTED:
//        ESP_LOGI(TAG, "Ethernet Link Down");
//        s_ethernet_is_connected = false;
//        ESP_ERROR_CHECK(esp_wifi_stop());
//        break;
//    case ETHERNET_EVENT_START:
//        ESP_LOGI(TAG, "Ethernet Started");
//        break;
//    case ETHERNET_EVENT_STOP:
//        ESP_LOGI(TAG, "Ethernet Stopped");
//        break;
//    default:
//        break;
//    }
//}

// Event handler for Wi-Fi
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static uint8_t s_con_cnt = 0;
    switch (event_id) {
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station connected");
        if (!s_con_cnt) {
            s_sta_is_connected = true;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, pkt_ap2sta);
        }
        s_con_cnt++;
        break;
    case WIFI_EVENT_AP_STADISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi AP got a station disconnected");
        s_con_cnt--;
        if (!s_con_cnt) {
            s_sta_is_connected = false;
            esp_wifi_internal_reg_rxcb(WIFI_IF_AP, NULL);
        }
        break;
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "Wi-Fi Station has started");
//        esp_wifi_get_mac(WIFI_IF_STA, s_eth_mac);
//        uint8_t *mac = s_eth_mac;
//        ESP_LOGE(TAG, "WIFI mac address: %x %x %x %x %x %x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
//
//        esp_wifi_set_mac(WIFI_IF_AP, s_eth_mac);
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "Wi-Fi Station has connected");
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, pkt_sta2ap);
        s_ethernet_is_connected = true;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        ESP_LOGI(TAG, "Wi-Fi Station has disconnected");
        s_ethernet_is_connected = false;
        esp_wifi_internal_reg_rxcb(WIFI_IF_STA, NULL);
        break;

    default:
        break;
    }
}

//static esp_err_t initialize_flow_control(void)
//{
//    flow_control_queue = xQueueCreate(FLOW_CONTROL_QUEUE_LENGTH, sizeof(flow_control_msg_t));
//    if (!flow_control_queue) {
//        ESP_LOGE(TAG, "create flow control queue failed");
//        return ESP_FAIL;
//    }
//    BaseType_t ret = xTaskCreate(eth2wifi_flow_control_task, "flow_ctl", 2048, NULL, (tskIDLE_PRIORITY + 2), NULL);
//    if (ret != pdTRUE) {
//        ESP_LOGE(TAG, "create flow control task failed");
//        return ESP_FAIL;
//    }
//    return ESP_OK;
//}
static esp_netif_t *wifi_init_softap(void)
{
//    esp_netif_t *esp_netif_ap = esp_netif_create_default_wifi_ap();

    wifi_config_t wifi_ap_config = {
            .ap = {
                    .ssid = CONFIG_EXAMPLE_WIFI_SSID,
                    .ssid_len = strlen(CONFIG_EXAMPLE_WIFI_SSID),
                    .password = CONFIG_EXAMPLE_WIFI_PASSWORD,
                    .max_connection = CONFIG_EXAMPLE_MAX_STA_CONN,
                    .authmode = WIFI_AUTH_WPA_WPA2_PSK,
                    .channel = CONFIG_EXAMPLE_WIFI_CHANNEL // default: channel 1
            },
    };

    if (strlen(CONFIG_EXAMPLE_WIFI_PASSWORD) == 0) {
        wifi_ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_ap_config));

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD, CONFIG_EXAMPLE_WIFI_CHANNEL);

    return NULL;
}
static esp_netif_t *wifi_init_sta(void)
{
//    esp_netif_t *esp_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_sta_config = {
            .sta = {
                    .ssid = CONFIG_EXAMPLE_WIFI_STA_SSID,
                    .password = CONFIG_EXAMPLE_WIFI_STA_PASSWORD,
                    .scan_method = WIFI_ALL_CHANNEL_SCAN,
                    .failure_retry_cnt = CONFIG_EXAMPLE_MAXIMUM_STA_RETRY
            },
    };

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_sta_config) );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    return NULL;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,NULL));

    /*Initialize WiFi */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_read_mac(s_sta_mac, ESP_MAC_WIFI_STA);
    esp_read_mac(s_eth_mac, ESP_MAC_WIFI_SOFTAP);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* Initialize AP */
    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
    wifi_init_softap();

    /* Initialize STA */
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    /* Start WiFi */
    ESP_ERROR_CHECK(esp_wifi_start() );
//    initialize_wifi();
//    initialize_ethernet();
}


/**
 * Set this to 1 to runtime update HW addresses in DHCP messages
 * (this is needed if the client uses 61 option and the DHCP server applies strict rules on assigning addresses)
 */
#define MODIFY_DHCP_MSGS        0
/**
 *  In this scenario of WiFi station to Ethernet bridge mode, we have this configuration
 *
 *   (ISP) router        ESP32               PC
 *      [ AP ] <->   [ sta -- eth ] <->  [ eth-NIC ]
 *
 *  From the PC's NIC perspective the L2 forwarding should be transparent and resemble this configuration:
 *
 *   (ISP) router                           PC
 *      [ AP ]       <---------->       [ virtual wifi-NIC ]
 *
 *  In order for the ESP32 to act as L2 bridge it needs to accept all frames on the interface
 *  - For Ethernet we just enable `PROMISCUOUS` mode
 *  - For Wifi we could also enable the promiscuous mode, but in that case we'd receive encoded frames
 *    from 802.11 and we'd have to decode it and process (using wpa-supplicant).
 *    The easier option (in this scenario of only one client -- eth-NIC) we could simply "pretend"
 *    that we have the HW mac address of eth-NIC and receive only ethernet frames for "us" from esp_wifi API
 *
 *  This API updates Ethernet frames to swap mac addresses of ESP32 interfaces with those of eth-NIC and AP.
 *  For that we'd have to parse initial DHCP packets (manually) to record the HW addresses of the AP and eth-NIC
 *  (note, that it is possible to simply spoof the MAC addresses, but that's not recommended technique)
 */
#define IP_V4 0x40
#define IP_PROTO_UDP 0x11
#define DHCP_PORT_IN 0x43
#define DHCP_PORT_OUT 0x44
#define DHCP_MACIG_COOKIE_OFFSET (8 + 236)
#define DHCP_HW_ADDRESS_OFFSET (36)
#define MIN_DHCP_PACKET_SIZE (285)
#define IP_HEADER_SIZE (20)
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_COOKIE_WITH_PKT_TYPE(type) {0x63, 0x82, 0x53, 0x63, 0x35, 1, type};

#if MODIFY_DHCP_MSGS
static void update_udp_checksum(uint16_t *udp_header, uint16_t* ip_header)
{
    uint32_t sum = 0;
    uint16_t *ptr = udp_header;
    ptr[3] = 0; // clear the current checksum
    int payload_len = htons(ip_header[1]) - IP_HEADER_SIZE;
    // add UDP payload
    for (int i = 0; i < payload_len/2; i++) {
        sum += htons(*ptr++);
    }
    // add the padding if the packet length is odd
    if (payload_len & 1) {
        sum += (*((uint8_t *)ptr) << 8);
    }
    // add some IP header data
    ptr = ip_header + 6;
    for (int i = 0; i < 4; i++) {       // IP addresses
        sum += htons(*ptr++);
    }
    sum += IP_PROTO_UDP + payload_len;  // protocol + size
    do {
        sum = (sum & 0xFFFF) + (sum >> 16);
    } while (sum & 0xFFFF0000);         //  process the carry
    ptr = udp_header;
    ptr[3] = htons(~sum);   // update the UDP header with the new checksum
}
#endif // MODIFY_DHCP_MSGS

void mac_spoof(mac_spoof_direction_t direction, uint8_t *buffer, uint16_t len, uint8_t own_mac[6])
{
    if (!s_ethernet_is_connected) {
        return;
    }
    static uint8_t eth_nic_mac[6] = {};
    static bool eth_nic_mac_found = false;
    static uint8_t ap_mac[6] = {};
    static bool ap_mac_found = false;

    uint8_t *dest_mac = buffer;
    uint8_t *src_mac = buffer + 6;
    uint8_t *eth_type = buffer + 12;
    if (eth_type[0] == 0x08) {      // support only IPv4
        // try to find NIC HW address (look for DHCP discovery packet)
        if ( (!eth_nic_mac_found || (MODIFY_DHCP_MSGS)) && direction == FROM_WIRED && eth_type[1] == 0x00) {  // ETH IP4
            uint8_t *ip_header = eth_type + 2;
            if (len > MIN_DHCP_PACKET_SIZE && (ip_header[0] & 0xF0) == IP_V4 && ip_header[9] == IP_PROTO_UDP) {
                uint8_t *udp_header = ip_header + IP_HEADER_SIZE;
                const uint8_t dhcp_ports[] = {0, DHCP_PORT_OUT, 0, DHCP_PORT_IN};
                if (memcmp(udp_header, dhcp_ports, sizeof(dhcp_ports)) == 0) {
                    uint8_t *dhcp_magic = udp_header + DHCP_MACIG_COOKIE_OFFSET;
                    const uint8_t dhcp_type[] = DHCP_COOKIE_WITH_PKT_TYPE(DHCP_DISCOVER);
                    if (!eth_nic_mac_found && memcmp(dhcp_magic, dhcp_type, sizeof(dhcp_type)) == 0) {
                        eth_nic_mac_found = true;
                        memcpy(eth_nic_mac, src_mac, 6);
                    }
#if MODIFY_DHCP_MSGS
                    if (eth_nic_mac_found) {
                        bool update_checksum = false;
                        // Replace the BOOTP HW address
                        uint8_t *dhcp_client_hw_addr = udp_header + DHCP_HW_ADDRESS_OFFSET;
                        if (memcmp(dhcp_client_hw_addr, eth_nic_mac, 6) == 0) {
                            memcpy(dhcp_client_hw_addr, own_mac, 6);
                            update_checksum = true;
                        }
                        // Replace the HW address in opt-61
                        uint8_t *dhcp_opts = dhcp_magic + 4;
                        while (*dhcp_opts != 0xFF) {
                            if (dhcp_opts[0] == 61 && dhcp_opts[1] == 7 /* size (type=1 + mac=6) */ && dhcp_opts[2] == 1 /* HW address type*/ &&
                                memcmp(dhcp_opts + 3, eth_nic_mac, 6) == 0) {
                                update_checksum = true;
                                memcpy(dhcp_opts + 3, own_mac, 6);
                                break;
                            }
                            dhcp_opts += dhcp_opts[1]+ 2;
                            if (dhcp_opts - buffer >= len) {
                                break;
                            }
                        }
                        if (update_checksum) {
                            update_udp_checksum((uint16_t *) udp_header, (uint16_t *) ip_header);
                        }
                    }
#endif // MODIFY_DHCP_MSGS
                }   // DHCP
            } // UDP/IP
#if MODIFY_DHCP_MSGS
            // try to find AP HW address (look for DHCP offer packet)
        } else if ( (!ap_mac_found || (MODIFY_DHCP_MSGS)) && direction == TO_WIRED && eth_type[1] == 0x00) {  // ETH IP4
            uint8_t *ip_header = eth_type + 2;
            if (len > MIN_DHCP_PACKET_SIZE && (ip_header[0] & 0xF0) == IP_V4 && ip_header[9] == IP_PROTO_UDP) {
                uint8_t *udp_header = ip_header + IP_HEADER_SIZE;
                const uint8_t dhcp_ports[] = {0, DHCP_PORT_IN, 0, DHCP_PORT_OUT};
                if (memcmp(udp_header, dhcp_ports, sizeof(dhcp_ports)) == 0) {
                    uint8_t *dhcp_magic = udp_header + DHCP_MACIG_COOKIE_OFFSET;
                    if (eth_nic_mac_found) {
                        uint8_t *dhcp_client_hw_addr = udp_header + DHCP_HW_ADDRESS_OFFSET;
                        // Replace BOOTP HW address
                        if (memcmp(dhcp_client_hw_addr, own_mac, 6) == 0) {
                            memcpy(dhcp_client_hw_addr, eth_nic_mac, 6);
                            update_udp_checksum((uint16_t*)udp_header, (uint16_t*)ip_header);
                        }
                    }
                    const uint8_t dhcp_type[] = DHCP_COOKIE_WITH_PKT_TYPE(DHCP_OFFER);
                    if (!ap_mac_found && memcmp(dhcp_magic, dhcp_type, sizeof(dhcp_type)) == 0) {
                        ap_mac_found = true;
                        memcpy(ap_mac, src_mac, 6);
                    }
                }   // DHCP
            } // UDP/IP
#endif //  MODIFY_DHCP_MSGS
        }

        // swap addresses in ARP probes
        if (eth_type[1] == 0x06) { // ARP
            uint8_t *arp = eth_type + 2 + 8; // points to sender's HW address
            if (eth_nic_mac_found && direction == FROM_WIRED && memcmp(arp, eth_nic_mac, 6) == 0) {
                /* updates senders HW address to our wireless */
                memcpy(arp, own_mac, 6);
            } else if (ap_mac_found && direction == TO_WIRED && memcmp(arp, ap_mac, 6) == 0) {
                /* updates senders HW address to our wired */
                memcpy(arp, s_eth_mac, 6);
            }
        }
        // swap HW addresses in ETH frames
        if (ap_mac_found && direction == FROM_WIRED && memcmp(dest_mac, s_eth_mac, 6) == 0) {
            memcpy(dest_mac, ap_mac, 6);
        }
        if (ap_mac_found && direction == TO_WIRED && memcmp(src_mac, ap_mac, 6) == 0) {
            memcpy(src_mac, s_eth_mac, 6);
        }
        if (eth_nic_mac_found && direction == FROM_WIRED && memcmp(src_mac, eth_nic_mac, 6) == 0) {
            memcpy(src_mac, own_mac, 6);
        }
        if (eth_nic_mac_found && direction == TO_WIRED && memcmp(dest_mac, own_mac, 6) == 0) {
            memcpy(dest_mac, eth_nic_mac, 6);
        }
    }   // IP4 section of eth-type (0x08) both ETH-IP4 and ETHARP
}
