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
#include "driver/spi_slave_hd.h"

#define QUEUE_SIZE 4
#define GPIO_MOSI 11
#define GPIO_MISO 13
#define GPIO_SCLK 12
#define GPIO_CS   10
#define DMA_CHAN   SPI_DMA_CH_AUTO

#define SLAVE_HOST SPI2_HOST

struct spihd_regs {
    uint16_t tx_size;
    uint8_t magic;
    uint8_t tx_count;
    uint8_t rx_count;
    uint8_t checksum;
} __attribute__((packed));

static struct spihd_regs s_regs = { .tx_size = -1 };

//#define SLAVE_READY_FLAG_REG            0
//#define SLAVE_READY_FLAG                0xEE
////Value in these 4 registers (Byte 4, 5, 6, 7) indicates the MAX Slave TX buffer length
//#define SLAVE_MAX_TX_BUF_LEN_REG        4
////Value in these 4 registers indicates the MAX Slave RX buffer length
//#define SLAVE_MAX_RX_BUF_LEN_REG        8
////Value in these 4 registers indicates size of the TX buffer that Slave has loaded to the DMA
//#define SLAVE_TX_READY_BUF_SIZE_REG     12
//
//#define SLAVE_RX_READY_FLAG_REG      20
//#define SLAVE_TX_READY_FLAG_REG      21


static const char *TAG = "ppp_slave";

static const int CONNECT_BIT = BIT0;
static EventGroupHandle_t event_group = NULL;
QueueHandle_t s_tx_queue;
esp_netif_t *s_netif;
static uint8_t s_tx_frames = 0;
static uint8_t s_rx_frames = 0;

struct wifi_buf {
    void *buffer;
    uint16_t len;
    void *eb;
    uint8_t count;
};

static esp_err_t transmit(void *h, void *buffer, size_t len)
{
    static uint8_t cnt = 0;
//    ESP_LOG_BUFFER_HEXDUMP("ppp_connect_tx", buffer, len, ESP_LOG_ERROR);
//    ESP_LOGE(TAG, "len=%d\n", len);
//    printf("O%d|\n", len);

#if CONFIG_EXAMPLE_CONNECT_PPP_DEVICE_SPI
    struct wifi_buf buf = { .buffer = malloc(len), .len = len, .count =cnt++};
    memcpy(buf.buffer, buffer, len);

    BaseType_t ret = xQueueSend(s_tx_queue, &buf, pdMS_TO_TICKS(100));
    return ret == pdTRUE ? ESP_OK : ESP_FAIL;
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
static void spi_sender(void *arg)
{
    esp_err_t err = ESP_OK;
    uint32_t send_buf_size = *(uint32_t *)arg;      //Tx buffer max size
    uint8_t *send_buf;                              //TX buffer
    spi_slave_hd_data_t slave_trans;                //Transaction descriptor
    spi_slave_hd_data_t *ret_trans;                 //Pointer to the descriptor of return result

    send_buf = heap_caps_calloc(1, send_buf_size, MALLOC_CAP_DMA);
    if (!send_buf) {
        ESP_LOGE(TAG, "No enough memory!");
        abort();
    }

    while (1) {
            /**
             * Here we simply get some random data.
             * In your own code, you could prepare some buffers, and create a FreeRTOS task to generate/get data, and give a
             * Semaphore to unblock this `sender()` Task.
             */
            struct wifi_buf buf;
            BaseType_t ret = xQueueReceive(s_tx_queue, &buf, pdMS_TO_TICKS(10));
            if (ret == pdTRUE) {
                ESP_LOGD(TAG, "Got packet size=%d, count=%d", buf.len, buf.count);
                slave_trans.data = buf.buffer;
                slave_trans.len = buf.len;
                slave_trans.arg = (void*)((intptr_t)buf.count);
                //Due to the `queue_sent_cnt` and `queue_recv_cnt` logic above, we are sure there is space to send data, this will return ESP_OK immediately
                ESP_ERROR_CHECK(spi_slave_hd_queue_trans(SLAVE_HOST, SPI_SLAVE_CHAN_TX, &slave_trans, portMAX_DELAY));
                // tell the master that we have something
//                s_tx_frames++;
//                ESP_LOGW(TAG, "s_tx_frames=%d\n", (int)s_tx_frames);
//                spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_TX_READY_FLAG_REG, (uint8_t *)&s_tx_frames, 1);
            }
        //Recycle the transaction
        while (1) {
            /**
             * Get the TX transaction result
             *
             * The ``ret_trans`` will exactly point to the transaction descriptor passed to the driver before (here ``slave_trans``).
             * For TX, the ``ret_trans->trans_len`` is meaningless. But you do need this API to maintain the internal queue.
             */
            err = spi_slave_hd_get_trans_res(SLAVE_HOST, SPI_SLAVE_CHAN_TX, &ret_trans, pdMS_TO_TICKS(0));
            if (err != ESP_OK) {
                assert(err == ESP_ERR_TIMEOUT);
//                vTaskDelay(pdMS_TO_TICKS(10));
//                if (ret == pdTRUE) {
//                    vTaskDelay(pdMS_TO_TICKS(10));
//                    continue;
//                }
//                ESP_LOGW(TAG, "continue...?");
                break; // CONTINUE?
            }
            if (ret_trans && ret_trans->arg) {
                free(ret_trans->data);
//                ESP_LOGE(TAG, "Completed Tx (total cnt=%d)", (int)(ret_trans->arg));
                ret_trans->arg = NULL;
                break;
            }
        }
    }
}

static void set_regs(void)
{
    uint8_t checksum = 0;
    s_regs.checksum = 0;
    for (int i=0; i< sizeof(s_regs); ++i)
        checksum += ((uint8_t*)&s_regs)[i];
    s_regs.checksum = checksum;
    spi_slave_hd_write_buffer(SLAVE_HOST, 0, (uint8_t *)&s_regs, sizeof(s_regs));
}

static void spi_receiver(void *arg)
{
    spi_slave_hd_data_t *ret_trans;
    uint32_t recv_buf_size = *(uint32_t *)arg;
    uint8_t *recv_buf;
    spi_slave_hd_data_t slave_trans;
    recv_buf = heap_caps_calloc(1, recv_buf_size, MALLOC_CAP_DMA);
    if (!recv_buf) {
        ESP_LOGE(TAG, "No enough memory!");
        abort();
    }

    slave_trans.data = recv_buf;
    slave_trans.len = recv_buf_size;
    ESP_ERROR_CHECK(spi_slave_hd_queue_trans(SLAVE_HOST, SPI_SLAVE_CHAN_RX, &slave_trans, portMAX_DELAY));
    s_regs.rx_count = 1;
    set_regs();
//    s_rx_frames = 1;
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_RX_READY_FLAG_REG, (uint8_t *)&s_rx_frames, 1);

    while (1) {
        ESP_ERROR_CHECK(spi_slave_hd_get_trans_res(SLAVE_HOST, SPI_SLAVE_CHAN_RX, &ret_trans, portMAX_DELAY));
        //Process the received data in your own code. Here we just print it out.
//        ESP_LOG_BUFFER_HEXDUMP("ppp_uart_recv", ret_trans->data, ret_trans->trans_len, ESP_LOG_WARN);
//        printf("I%d|\n", ret_trans->trans_len);
        esp_netif_receive(s_netif, ret_trans->data, ret_trans->trans_len, NULL);
//        ESP_LOGW(TAG, "received... %d", s_rx_frames);
        /**
         * Prepared data for new transaction
         */
        slave_trans.data = recv_buf;
        slave_trans.len = recv_buf_size;
        ESP_ERROR_CHECK(spi_slave_hd_queue_trans(SLAVE_HOST, SPI_SLAVE_CHAN_RX, &slave_trans, portMAX_DELAY));

    }
}


static bool cb_set_tx_ready_buf_size(void *arg, spi_slave_hd_event_t *event, BaseType_t *awoken)
{
    uint32_t s_tx_ready_buf_size = event->trans->len;
    s_regs.tx_count++;
    s_regs.tx_size = s_tx_ready_buf_size;
    set_regs();
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_TX_READY_BUF_SIZE_REG, (uint8_t *)&s_tx_ready_buf_size, 4);
//    s_tx_frames++;
//                ESP_LOGW(TAG, "s_tx_frames=%d\n", (int)s_tx_frames);
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_TX_READY_FLAG_REG, (uint8_t *)&s_tx_frames, 1);

    return true;
}
static bool cb_set_rx_ready_buf_num(void *arg, spi_slave_hd_event_t *event, BaseType_t *awoken)
{
//    s_rx_frames++;
//
    s_regs.rx_count++;
    set_regs();
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_RX_READY_FLAG_REG, (uint8_t *)&s_rx_frames, 1);
    return true;
}

static void init_slave_hd(void)
{    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = GPIO_MOSI;
    bus_cfg.miso_io_num = GPIO_MISO;
    bus_cfg.sclk_io_num = GPIO_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 14000;
    bus_cfg.flags = 0;
    bus_cfg.intr_flags = 0;

    spi_slave_hd_slot_config_t slave_hd_cfg = {};
    slave_hd_cfg.spics_io_num = GPIO_CS;
    slave_hd_cfg.flags = 0;
    slave_hd_cfg.mode = 0;
    slave_hd_cfg.command_bits = 8;
    slave_hd_cfg.address_bits = 8;
    slave_hd_cfg.dummy_bits = 8;
    slave_hd_cfg.queue_size = QUEUE_SIZE;
    slave_hd_cfg.dma_chan = DMA_CHAN;
    slave_hd_cfg.cb_config = (spi_slave_hd_callback_config_t) {
            .cb_send_dma_ready = cb_set_tx_ready_buf_size,
            .cb_recv_dma_ready = cb_set_rx_ready_buf_num,
    };

    ESP_ERROR_CHECK(spi_slave_hd_init(SLAVE_HOST, &bus_cfg, &slave_hd_cfg));

    s_tx_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct wifi_buf));
    uint8_t init_value[SOC_SPI_MAXIMUM_BUFFER_SIZE] = {0x0};
    spi_slave_hd_write_buffer(SLAVE_HOST, 0, init_value, SOC_SPI_MAXIMUM_BUFFER_SIZE);
    static uint32_t send_buf_size = 1600;
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_MAX_TX_BUF_LEN_REG, (uint8_t *)&send_buf_size, sizeof(send_buf_size));
    static uint32_t recv_buf_size = 1600;
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_MAX_RX_BUF_LEN_REG, (uint8_t *)&recv_buf_size, sizeof(recv_buf_size));
//    uint32_t slave_ready_flag = SLAVE_READY_FLAG;
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_READY_FLAG_REG, (uint8_t *)&slave_ready_flag, sizeof(slave_ready_flag));
    xTaskCreate(spi_sender, "sendTask", 4096, &send_buf_size, 18, NULL);
    xTaskCreate(spi_receiver, "recvTask", 4096, &recv_buf_size, 18, NULL);
    set_regs();
    vTaskDelay(pdMS_TO_TICKS(2000));
    s_regs.magic = 0x5A;
    set_regs();

//    slave_ready_flag = 0;
//    spi_slave_hd_write_buffer(SLAVE_HOST, SLAVE_READY_FLAG_REG, (uint8_t *)&slave_ready_flag, sizeof(slave_ready_flag));

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
    init_slave_hd();
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
