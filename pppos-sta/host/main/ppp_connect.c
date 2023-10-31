/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_netif_ppp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_serial_slave_link/essl_spi.h"
#define GPIO_MOSI    11
#define GPIO_MISO    13
#define GPIO_SCLK    12
#define GPIO_CS      10
#define MASTER_HOST SPI2_HOST
#define DMA_CHAN     SPI_DMA_CH_AUTO
#define SLAVE_READY_FLAG_REG            0
#define SLAVE_READY_FLAG                0xEE
//Value in these 4 registers (Byte 4, 5, 6, 7) indicates the MAX Slave TX buffer length
#define SLAVE_MAX_TX_BUF_LEN_REG        4
//Value in these 4 registers indicates the MAX Slave RX buffer length
#define SLAVE_MAX_RX_BUF_LEN_REG        8

//----------------------Updating Info------------------------//
//Value in these 4 registers indicates size of the TX buffer that Slave has loaded to the DMA
#define SLAVE_TX_READY_BUF_SIZE_REG     12
//Value in these 4 registers indicates number of the RX buffer that Slave has loaded to the DMA
#define SLAVE_RX_READY_BUF_NUM_REG      16

#define SLAVE_RX_READY_FLAG_REG      20
#define SLAVE_TX_READY_FLAG_REG      21

#define TX_SIZE_MIN  40

static     spi_device_handle_t spi;
static uint8_t s_tx_frames = -1;
static uint8_t s_rx_frames = 0;

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB
#include "tinyusb.h"
#include "tusb_cdc_acm.h"

static int s_itf;
static uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE];

#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB

#include "driver/uart.h"
#define BUF_SIZE (1024)
static bool s_stop_task = false;

#endif // CONNECT_PPP_DEVICE

struct wifi_buf {
    void *buffer;
    uint16_t len;
    void *eb;
};
static esp_netif_t *s_netif = NULL;
QueueHandle_t s_tx_queue;


static const char *TAG = "example_connect_ppp";
static int s_retry_num = 0;
static EventGroupHandle_t s_event_group = NULL;
static esp_netif_t *s_netif;
static const int GOT_IPV4 = BIT0;
static const int CONNECTION_FAILED = BIT1;
#if CONFIG_EXAMPLE_CONNECT_IPV6
static const int GOT_IPV6 = BIT2;
#define CONNECT_BITS (GOT_IPV4|GOT_IPV6|CONNECTION_FAILED)
#else
#define CONNECT_BITS (GOT_IPV4|CONNECTION_FAILED)
#endif

static esp_err_t transmit(void *h, void *buffer, size_t len)
{
    ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, len, ESP_LOG_VERBOSE);
#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
    struct wifi_buf buf = { .buffer = malloc(len), .len = len};
    memcpy(buf.buffer, buffer, len);

    BaseType_t ret = xQueueSend(s_tx_queue, &buf, pdMS_TO_TICKS(100));
    if (ret != pdTRUE) {
        ESP_LOGE(TAG, "Failed to queue packet to slave!");
    }
    return ESP_OK;
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB
    tinyusb_cdcacm_write_queue(s_itf, buffer, len);
    tinyusb_cdcacm_write_flush(s_itf, 0);
#else // DEVICE_UART
    uart_write_bytes(UART_NUM_1, buffer, len);
#endif // CONNECT_PPP_DEVICE
    return ESP_OK;
}

static esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,    // singleton driver, just to != NULL
        .transmit = transmit,
};
const esp_netif_driver_ifconfig_t *ppp_driver_cfg = &driver_cfg;

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        esp_netif_t *netif = event->esp_netif;
        esp_netif_dns_info_t dns_info;
        ESP_LOGI(TAG, "Got IPv4 event: Interface \"%s\" address: " IPSTR, esp_netif_get_desc(event->esp_netif), IP2STR(&event->ip_info.ip));
        esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info);
        ESP_LOGI(TAG, "Main DNS server : " IPSTR, IP2STR(&dns_info.ip.u_addr.ip4));
        xEventGroupSetBits(s_event_group, GOT_IPV4);
#if CONFIG_EXAMPLE_CONNECT_IPV6
    } else if (event_id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        if (!example_is_our_netif(EXAMPLE_NETIF_DESC_PPP, event->esp_netif)) {
            return;
        }
        esp_ip6_addr_type_t ipv6_type = esp_netif_ip6_get_addr_type(&event->ip6_info.ip);
        ESP_LOGI(TAG, "Got IPv6 event: Interface \"%s\" address: " IPV6STR ", type: %s", esp_netif_get_desc(event->esp_netif),
                 IPV62STR(event->ip6_info.ip), example_ipv6_addr_types_to_str[ipv6_type]);
        if (ipv6_type == EXAMPLE_CONNECT_PREFERRED_IPV6_TYPE) {
            xEventGroupSetBits(s_event_group, GOT_IPV6);
        }
#endif
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Disconnect from PPP Server");
        s_retry_num++;
        if (s_retry_num > CONFIG_EXAMPLE_PPP_CONN_MAX_RETRY) {
            ESP_LOGE(TAG, "PPP Connection failed %d times, stop reconnecting.", s_retry_num);
            xEventGroupSetBits(s_event_group, CONNECTION_FAILED);
        } else {
            ESP_LOGI(TAG, "PPP Connection failed %d times, try to reconnect.", s_retry_num);
            esp_netif_action_start(s_netif, 0, 0, 0);
            esp_netif_action_connected(s_netif, 0, 0, 0);
        }

    }
}

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB
static void cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    if (itf != s_itf) {
        // Not our channel
        return;
    }
    esp_err_t ret = tinyusb_cdcacm_read(itf, buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK) {
        ESP_LOG_BUFFER_HEXDUMP(TAG, buf, rx_size, ESP_LOG_VERBOSE);
        // pass the received data to the network interface
        esp_netif_receive(s_netif, buf, rx_size, NULL);
    } else {
        ESP_LOGE(TAG, "Read error");
    }
}

static void line_state_changed(int itf, cdcacm_event_t *event)
{
    s_itf = itf; // use this channel for the netif communication
    ESP_LOGI(TAG, "Line state changed on channel %d", itf);
}
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_UART
static void ppp_task(void *args)
{
    uart_config_t uart_config = {};
    uart_config.baud_rate = CONFIG_EXAMPLE_CONNECT_UART_BAUDRATE;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    QueueHandle_t event_queue;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE, 0, 16, &event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, CONFIG_EXAMPLE_CONNECT_UART_TX_PIN, CONFIG_EXAMPLE_CONNECT_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_1, 1));

    char *buffer = (char*)malloc(BUF_SIZE);
    uart_event_t event;
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected, s_netif);
    esp_netif_ppp_config_t netif_params;
    ESP_ERROR_CHECK(esp_netif_ppp_get_params(s_netif, &netif_params));
    netif_params.ppp_our_ip4_addr = ESP_IP4TOADDR(192,168,11,2);
    netif_params.ppp_their_ip4_addr = ESP_IP4TOADDR(192,168,11,1);
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(s_netif, &netif_params));

    esp_netif_action_start(s_netif, 0, 0, 0);
    esp_netif_action_connected(s_netif, 0, 0, 0);
    while (!s_stop_task) {
        xQueueReceive(event_queue, &event, pdMS_TO_TICKS(1000));
        if (event.type == UART_DATA) {
            size_t len;
            uart_get_buffered_data_len(UART_NUM_1, &len);
            if (len) {
                len = uart_read_bytes(UART_NUM_1, buffer, BUF_SIZE, 0);
                ESP_LOG_BUFFER_HEXDUMP(TAG, buffer, len, ESP_LOG_VERBOSE);
                esp_netif_receive(s_netif, buffer, len, NULL);
            }
        } else {
            ESP_LOGW(TAG, "Received UART event: %d", event.type);
        }
    }
    free(buffer);
    vTaskDelete(NULL);
}
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI

static void init_master_hd(spi_device_handle_t* out_spi)
{
    //init bus
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = GPIO_MOSI;
    bus_cfg.miso_io_num = GPIO_MISO;
    bus_cfg.sclk_io_num = GPIO_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 14000;
    bus_cfg.flags = 0;
    bus_cfg.intr_flags = 0;

    ESP_ERROR_CHECK(spi_bus_initialize(MASTER_HOST, &bus_cfg, DMA_CHAN));

    //add device
    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = 10*1000*1000;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = GPIO_CS;
    dev_cfg.cs_ena_pretrans = 0;
    dev_cfg.cs_ena_posttrans = 0;
    dev_cfg.command_bits = 8;
    dev_cfg.address_bits = 8;
    dev_cfg.dummy_bits = 8;
    dev_cfg.queue_size = 16;
    dev_cfg.flags = SPI_DEVICE_HALFDUPLEX;
    dev_cfg.duty_cycle_pos = 0;
    dev_cfg.input_delay_ns = 0;
    dev_cfg.pre_cb = NULL;
    dev_cfg.post_cb = NULL;
    ESP_ERROR_CHECK(spi_bus_add_device(MASTER_HOST, &dev_cfg, out_spi));

    s_tx_queue = xQueueCreate(16, sizeof(struct wifi_buf));
    esp_err_t ret;
    uint32_t slave_ready_flag;

    while (1) {
        //Master sends CMD2 to get slave configuration
        //The first byte is a flag assigned by slave as a start signal, here it's 0xee
        ret = essl_spi_rdbuf(spi, (uint8_t *)&slave_ready_flag, SLAVE_READY_FLAG_REG, 4, 0);
        if (ret != ESP_OK) {
            abort();
        }

        if (slave_ready_flag != SLAVE_READY_FLAG) {
            printf("Waiting for Slave to be ready...\n");
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        } else if (slave_ready_flag == SLAVE_READY_FLAG) {
            break;
        }
    }

    uint32_t slave_max_buf_size;
    ESP_ERROR_CHECK(essl_spi_rdbuf(spi, (uint8_t *)&slave_max_buf_size, SLAVE_MAX_TX_BUF_LEN_REG, 4, 0));
    printf("Slave MAX TX Buffer Size:       %"PRIu32"\n", slave_max_buf_size);
    ESP_ERROR_CHECK(essl_spi_rdbuf(spi, (uint8_t *)&slave_max_buf_size, SLAVE_MAX_RX_BUF_LEN_REG, 4, 0));
    printf("Slave MAX Rx Buffer Size:       %"PRIu32"\n", slave_max_buf_size);
    uint8_t *send_buf = heap_caps_calloc(1, slave_max_buf_size, MALLOC_CAP_DMA);
    if (!send_buf) {
        ESP_LOGE(TAG, "No enough memory!");
        abort();
    }

}

static uint32_t get_slave_tx_buf_size(spi_device_handle_t spi)
{
    uint32_t updated_size;
    uint32_t temp;
    ESP_ERROR_CHECK(essl_spi_rdbuf_polling(spi, (uint8_t *)&temp, SLAVE_TX_READY_BUF_SIZE_REG, 4, 0));
    /**
     * Read until the last 2 reading result are same. Reason:
     * SPI transaction is carried on per 1 Byte. So when Master is reading the shared register, if the
     * value is changed by Slave at this time, Master may get wrong data.
     */
    while (1) {
        ESP_ERROR_CHECK(essl_spi_rdbuf_polling(spi, (uint8_t *)&updated_size, SLAVE_TX_READY_BUF_SIZE_REG, 4, 0));
        if (updated_size == temp) {
            return updated_size;
        }
        temp = updated_size;
    }
}


static void ppp_task(void *args)
{
    uint8_t *recv_buf = heap_caps_calloc(1, 1600, MALLOC_CAP_DMA);

    while (1) {
        uint8_t temp;

        ESP_ERROR_CHECK(essl_spi_rdbuf_polling(spi, &temp, SLAVE_TX_READY_FLAG_REG, 1, 0));
        if (s_tx_frames != temp) {
            ESP_LOGD(TAG, "Slave has something %d %d", temp, s_tx_frames);
            uint32_t size_to_read = 0;
            s_tx_frames++;
            size_to_read = get_slave_tx_buf_size(spi);
            if (size_to_read > 0) {
                ESP_ERROR_CHECK(essl_spi_rddma(spi, recv_buf, size_to_read, -1, 0));
                ESP_LOGD(TAG, "RECEIVING......len=%d", (int)size_to_read);
                esp_netif_receive(s_netif, recv_buf, size_to_read, NULL);
            }

        } else {
            ESP_LOGD(TAG, "Slave doesn't have anything to say %d %d", temp, s_tx_frames);

        }

        struct wifi_buf buf;
        BaseType_t ret = xQueueReceive(s_tx_queue, &buf, pdMS_TO_TICKS(200));
        if (ret == pdTRUE) {

            ESP_LOGD(TAG, "Got packet ready to send... sending!");
            while (1) {
                ESP_ERROR_CHECK(essl_spi_rdbuf_polling(spi, &temp, SLAVE_RX_READY_FLAG_REG, 1, 0));
                if (s_rx_frames != temp) {
                    break;
                }
                ESP_LOGD(TAG, "Slave NOT ready  ... %d %d", temp, s_rx_frames);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            s_rx_frames++;
            ESP_LOGD(TAG, "Slave is ready %d %d!!", temp, s_rx_frames);
            ESP_ERROR_CHECK(essl_spi_wrdma(spi, buf.buffer, buf.len, -1, 0));

        }
    }
}

#endif // CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_UART or USB or SPI

esp_err_t example_ppp_connect(void)
{
    ESP_LOGI(TAG, "Start example_connect.");

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB
    ESP_LOGI(TAG, "USB initialization");
    const tinyusb_config_t tusb_cfg = {
            .device_descriptor = NULL,
            .string_descriptor = NULL,
            .external_phy = false,
            .configuration_descriptor = NULL,
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
            .usb_dev = TINYUSB_USBDEV_0,
            .cdc_port = TINYUSB_CDC_ACM_0,
            .callback_rx = &cdc_rx_callback,
            .callback_rx_wanted_char = NULL,
            .callback_line_state_changed = NULL,
            .callback_line_coding_changed = NULL
    };

    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));
    /* the second way to register a callback */
    ESP_ERROR_CHECK(tinyusb_cdcacm_register_callback(
            TINYUSB_CDC_ACM_0,
            CDC_EVENT_LINE_STATE_CHANGED,
            &line_state_changed));
#endif // CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB

    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));

    esp_netif_inherent_config_t base_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    base_netif_cfg.if_desc = "pppos";
    esp_netif_config_t netif_ppp_config = { .base = &base_netif_cfg,
            .driver = ppp_driver_cfg,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP
    };

    s_netif = esp_netif_new(&netif_ppp_config);
    assert(s_netif);
#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
    init_master_hd(&spi);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected, s_netif);
    esp_netif_ppp_config_t netif_params;
    ESP_ERROR_CHECK(esp_netif_ppp_get_params(s_netif, &netif_params));
    netif_params.ppp_our_ip4_addr = ESP_IP4TOADDR(192,168,11,2);
    netif_params.ppp_their_ip4_addr = ESP_IP4TOADDR(192,168,11,1);
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(s_netif, &netif_params));

    esp_netif_action_start(s_netif, 0, 0, 0);
    esp_netif_action_connected(s_netif, 0, 0, 0);
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_USB
    esp_netif_action_start(s_netif, 0, 0, 0);
    esp_netif_action_connected(s_netif, 0, 0, 0);
#else // DEVICE is UART
    s_stop_task = false;
    if (xTaskCreate(ppp_task, "ppp connect", 4096, NULL, 5, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create a ppp connection task");
        return ESP_FAIL;
    }
#endif // CONNECT_PPP_DEVICE

    if (xTaskCreate(ppp_task, "ppp connect", 4096, NULL, 5, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create a ppp connection task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Waiting for IP address");
    EventBits_t bits = xEventGroupWaitBits(s_event_group, CONNECT_BITS, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & CONNECTION_FAILED) {
        ESP_LOGE(TAG, "Connection failed!");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Connected!");
    // Setup global DNS
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = 0x08080808;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns));

    return ESP_OK;
}

