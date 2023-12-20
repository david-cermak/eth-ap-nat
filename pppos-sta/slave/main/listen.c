/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include <string.h>
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_netif_net_stack.h"
#include "esp_log.h"
#include "esp_event.h"
#include "sdkconfig.h"
#include "driver/uart.h"
#include "lwip/netif.h"
#include "lwip/lwip_napt.h"
//#include "driver/spi_slave_hd.h"
#include "driver/spi_slave.h"
#include "driver/gpio.h"

#define QUEUE_SIZE 4
#define GPIO_MOSI 11
#define GPIO_MISO 13
#define GPIO_SCLK 12
#define GPIO_CS   10
#define DMA_CHAN   SPI_DMA_CH_AUTO
#define GPIO_HANDSHAKE      2

#define SLAVE_HOST SPI2_HOST
#define MAX_PAYLOAD 1600
static const char *TAG = "ppp_slave";

static const int CONNECT_BIT = BIT0;
static EventGroupHandle_t event_group = NULL;
QueueHandle_t s_tx_queue;
esp_netif_t *s_netif;

struct wifi_buf {
    void *buffer;
    uint16_t len;
    void *eb;
    uint8_t count;
};

static esp_err_t transmit(void *h, void *buffer, size_t len)
{
//    static uint8_t cnt = 0;
//    ESP_LOG_BUFFER_HEXDUMP("ppp_connect_tx", buffer, len, ESP_LOG_ERROR);
//    ESP_LOGE(TAG, "len=%d\n", len);
//    printf("O%d|\n", len);

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
    struct wifi_buf buf = { };

    size_t remaining = len;
    do {
        size_t batch = remaining > MAX_PAYLOAD ? MAX_PAYLOAD : remaining;
        buf.buffer = malloc(batch);
        buf.len = batch;
        remaining -= batch;
        memcpy(buf.buffer, buffer, batch);
        BaseType_t ret = xQueueSend(s_tx_queue, &buf, pdMS_TO_TICKS(10));
        if (ret != pdTRUE) {
            ESP_LOGE(TAG, "Failed to queue packet to slave!");
        }
    } while (remaining > 0);

//    struct wifi_buf buf = { .buffer = malloc(len), .len = len, .count =cnt++};
//    memcpy(buf.buffer, buffer, len);
//
//    BaseType_t ret = xQueueSend(s_tx_queue, &buf, pdMS_TO_TICKS(100));
//    return ret == pdTRUE ? ESP_OK : ESP_FAIL;
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_UART
    uart_write_bytes(UART_NUM_1, buffer, len);
#endif
    return ESP_OK;
}

static esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = (void *)1,    // singleton driver, just to != NULL
        .transmit = transmit,
};

const esp_netif_driver_ifconfig_t *ppp_driver_cfg = &driver_cfg;

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_UART
#define BUF_SIZE (2*1024)
#define UART_TX_PIN 10
#define UART_RX_PIN 11

static void ppp_task(void *args)
{
    esp_netif_t* netif = (esp_netif_t*)args;
    bool stop_task = false;
    uart_config_t uart_config = {};
//    uart_config.baud_rate = 921600;
    uart_config.baud_rate = 3000000;
    uart_config.data_bits = UART_DATA_8_BITS;
    uart_config.parity    = UART_PARITY_DISABLE;
    uart_config.stop_bits = UART_STOP_BITS_1;
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = UART_SCLK_DEFAULT;

    QueueHandle_t event_queue;
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, BUF_SIZE, 0, 16, &event_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART_TX_PIN, UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_1, 1));

    char *buffer = (char*)malloc(BUF_SIZE);
    uart_event_t event;
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected, netif);
    esp_netif_ppp_config_t netif_params;
    ESP_ERROR_CHECK(esp_netif_ppp_get_params(netif, &netif_params));
    netif_params.ppp_our_ip4_addr = ESP_IP4TOADDR(192,168,11,1);
    netif_params.ppp_their_ip4_addr = ESP_IP4TOADDR(192,168,11,2);
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(netif, &netif_params));

    esp_netif_action_start(netif, 0, 0, 0);
    while (!stop_task) {
        xQueueReceive(event_queue, &event, pdMS_TO_TICKS(pdMS_TO_TICKS(100)));
        if (event.type == UART_DATA) {
            size_t len;
            uart_get_buffered_data_len(UART_NUM_1, &len);
            if (len) {
//                ESP_LOGW(TAG, "len=%d\n", len);
                len = uart_read_bytes(UART_NUM_1, buffer, BUF_SIZE, 0);
                ESP_LOG_BUFFER_HEXDUMP("ppp_uart_recv", buffer, len, ESP_LOG_VERBOSE);
                esp_netif_receive(netif, buffer, len, NULL);
            }
        } else {
            ESP_LOGW(TAG, "Received UART event: %d", event.type);
        }
    }
}
#endif

static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "IP event! %" PRIu32, event_id);

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));
        xEventGroupSetBits(event_group, CONNECT_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGI(TAG, "Disconnect from PPP Server");
    }
}

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
//Called after a transaction is queued and ready for pickup by master. We use this to set the handshake line high.
static void my_post_setup_cb(spi_slave_transaction_t *trans)
{
    gpio_set_level(GPIO_HANDSHAKE, 1);
//    ESP_EARLY_LOGE(TAG, "1");
}

//Called after transaction is sent/received. We use this to set the handshake line low.
static void my_post_trans_cb(spi_slave_transaction_t *trans)
{
    gpio_set_level(GPIO_HANDSHAKE, 0);
//    ESP_EARLY_LOGE(TAG, "0");
}

#define TRANSFER_SIZE (MAX_PAYLOAD + 4)
struct header {
    uint16_t size;
    uint8_t magic;
    uint8_t checksum;
} __attribute__((packed));

static void ppp_task(void *args)
{
    spi_slave_transaction_t t;
    memset(&t, 0, sizeof(t));

    //Create the semaphore.
    WORD_ALIGNED_ATTR uint8_t out_buf[TRANSFER_SIZE] = {};
    WORD_ALIGNED_ATTR uint8_t in_buf[TRANSFER_SIZE] = {};

    while (1) {
        struct wifi_buf buf = { .len = 0 };
        struct header head = { .magic = 0x5A, .size = 0, .checksum = 0};
        BaseType_t tx_queue_stat = xQueueReceive(s_tx_queue, &buf, 0);
        if (tx_queue_stat == pdTRUE && buf.buffer) {
            head.size = buf.len;
        }
        t.length = TRANSFER_SIZE * 8;
        t.tx_buffer = out_buf;
        t.rx_buffer = in_buf;
        for (int i=0; i<sizeof(struct header)-1; ++i)
            head.checksum += ((uint8_t*)&head)[i];
        memcpy(out_buf, &head, sizeof(struct header));
        if (head.size > 0) {
            memcpy(out_buf + sizeof(struct header), buf.buffer, head.size);
//            printf("O:%d|\n", head.size);
            free(buf.buffer);
        } else {
//            printf("*");
        };
        esp_err_t ret = spi_slave_transmit(SLAVE_HOST, &t, portMAX_DELAY);

        if (ret == ESP_OK) {
            struct header *head = (struct header*)in_buf;
            uint8_t checksum = 0;
            for (int i=0; i<sizeof(struct header)-1; ++i)
                checksum += in_buf[i];
            if (checksum != head->checksum) {
                ESP_LOGE(TAG, "Wrong checksum");
                continue;
            }
            if (head->magic != 0xA5) {
                ESP_LOGE(TAG, "Wrong magic");
                continue;
            }
            if (head->size > 0) {
//                printf("I:%d|\n", head->size);
                esp_netif_receive(s_netif, in_buf + sizeof(struct header), head->size, NULL);
            } else {
//                printf(".");
            }
        }
    }
}

static void init_slave(void)
{    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = GPIO_MOSI;
    bus_cfg.miso_io_num = GPIO_MISO;
    bus_cfg.sclk_io_num = GPIO_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 14000;
    bus_cfg.flags = 0;
    bus_cfg.intr_flags = 0;

    //Configuration for the SPI slave interface
    spi_slave_interface_config_t slvcfg = {
            .mode = 0,
            .spics_io_num = GPIO_CS,
            .queue_size = 3,
            .flags = 0,
            .post_setup_cb = my_post_setup_cb,
            .post_trans_cb = my_post_trans_cb
    };

    //Configuration for the handshake line
    gpio_config_t io_conf = {
            .intr_type = GPIO_INTR_DISABLE,
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = BIT64(GPIO_HANDSHAKE),
    };

    //Configure handshake line as output
    gpio_config(&io_conf);
    //Enable pull-ups on SPI lines so we don't detect rogue pulses when no master is connected.
    gpio_set_pull_mode(GPIO_MOSI, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_SCLK, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(GPIO_CS, GPIO_PULLUP_ONLY);

    //Initialize SPI slave interface
    esp_err_t ret = spi_slave_initialize(SLAVE_HOST, &bus_cfg, &slvcfg, SPI_DMA_CH_AUTO);
    assert(ret == ESP_OK);

    s_tx_queue = xQueueCreate(4*16, sizeof(struct wifi_buf));

}
#endif // CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI

void station_ppp_listen()
{
    event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));

    esp_netif_inherent_config_t base_netif_cfg = ESP_NETIF_INHERENT_DEFAULT_PPP();
    esp_netif_config_t netif_ppp_config = { .base = &base_netif_cfg,
            .driver = ppp_driver_cfg,
            .stack = ESP_NETIF_NETSTACK_DEFAULT_PPP
    };

    s_netif = esp_netif_new(&netif_ppp_config);
#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_UART
    if (xTaskCreate(ppp_task, "ppp connect", 4096, s_netif, 18, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create a ppp connection task");
        return;
    }
#elif CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
    init_slave();
    if (xTaskCreate(ppp_task, "ppp connect", 2*4096, s_netif, 18, NULL) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to create a ppp connection task");
        return;
    }
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, esp_netif_action_connected, s_netif);
    esp_netif_ppp_config_t netif_params;
    ESP_ERROR_CHECK(esp_netif_ppp_get_params(s_netif, &netif_params));
    netif_params.ppp_our_ip4_addr = ESP_IP4TOADDR(192,168,11,1);
    netif_params.ppp_their_ip4_addr = ESP_IP4TOADDR(192,168,11,2);
    ESP_ERROR_CHECK(esp_netif_ppp_set_params(s_netif, &netif_params));

    esp_netif_action_start(s_netif, 0, 0, 0);
    esp_netif_action_connected(s_netif, 0, 0, 0);
#else
#error Connection not supported
#endif
    ESP_LOGI(TAG, "Waiting for IP address");
    xEventGroupWaitBits(event_group, CONNECT_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
    ESP_LOGI(TAG, "Connected!");

    ESP_ERROR_CHECK(esp_netif_napt_enable(s_netif));
}
