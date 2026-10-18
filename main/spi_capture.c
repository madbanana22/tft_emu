#include "spi_capture.h"
#include "tft_emu.h"
#include "nvs_store.h"
#include "proto_st7735.h"
#include "panel_fb.h"

#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/portmacro.h>
#include <driver/gpio.h>
#include <driver/spi_slave.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <string.h>

static const char *TAG = "spi";

#define HOST CONFIG_TFT_EMU_SPI_HOST
#define QN CONFIG_TFT_EMU_TRANS_QUEUE
#define TBN ((size_t)CONFIG_TFT_EMU_TRANS_BYTES)

typedef struct {
    spi_slave_transaction_t t;
    uint8_t *buf;
} slot_t;

static slot_t s_slots[QN];

static portMUX_TYPE s_dc_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_dc_ring[16];
static volatile uint8_t s_dc_head, s_dc_tail;
static volatile bool s_res_pending;
static int64_t s_last_act;
static int64_t s_last_dump;
static int64_t s_last_mis;

static void IRAM_ATTR cs_fall_cb(void *arg)
{
    (void)arg;
    uint8_t d = gpio_get_level(CONFIG_TFT_EMU_PIN_DC);
    portENTER_CRITICAL_ISR(&s_dc_mux);
    s_dc_ring[s_dc_head & 15U] = d;
    s_dc_head++;
    if (s_dc_head - s_dc_tail > 16) s_dc_tail = s_dc_head - 16;
    portEXIT_CRITICAL_ISR(&s_dc_mux);
    g_stat.cs_edges++;
}

static void IRAM_ATTR res_pin_cb(void *arg)
{
    (void)arg;
    s_res_pending = true;
}

static uint8_t dc_pop(void)
{
    uint8_t v = 1;
    portENTER_CRITICAL(&s_dc_mux);
    if (s_dc_head != s_dc_tail) {
        v = s_dc_ring[s_dc_tail & 15U];
        s_dc_tail++;
    }
    portEXIT_CRITICAL(&s_dc_mux);
    return v;
}

void spi_capture_pins_rest(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_TFT_EMU_PIN_CS) | (1ULL << CONFIG_TFT_EMU_PIN_SCK) |
                        (1ULL << CONFIG_TFT_EMU_PIN_MOSI) | (1ULL << CONFIG_TFT_EMU_PIN_DC) |
                        (1ULL << CONFIG_TFT_EMU_TEST_PIN_SCK) | (1ULL << CONFIG_TFT_EMU_TEST_PIN_MOSI) |
                        (1ULL << CONFIG_TFT_EMU_TEST_PIN_CS) | (1ULL << CONFIG_TFT_EMU_TEST_PIN_DC),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
#if CONFIG_TFT_EMU_PIN_RES >= 0
    gpio_config_t ri = {
        .pin_bit_mask = 1ULL << CONFIG_TFT_EMU_PIN_RES,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&ri);
    gpio_isr_handler_add(CONFIG_TFT_EMU_PIN_RES, res_pin_cb, NULL);
#endif
    gpio_set_intr_type(CONFIG_TFT_EMU_PIN_CS, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(CONFIG_TFT_EMU_PIN_CS, cs_fall_cb, NULL);
}

static void capture_task(void *arg)
{
    (void)arg;

    for (int i = 0; i < QN; i++) {
        memset(&s_slots[i].t, 0, sizeof(s_slots[i].t));
        s_slots[i].t.length = TBN * 8;
        s_slots[i].t.rx_buffer = s_slots[i].buf;
        if (spi_slave_queue_trans(HOST, &s_slots[i].t, portMAX_DELAY) != ESP_OK) {
            ESP_LOGE(TAG, "queue slot %d failed", i);
        }
    }

    for (;;) {
        if (s_res_pending) {
            s_res_pending = false;
            ESP_LOGW(TAG, "host reset line asserted");
            proto_reset();
        }
        int64_t now = esp_timer_get_time();
        spi_slave_transaction_t *out;
        if (spi_slave_get_trans_result(HOST, &out, pdMS_TO_TICKS(20)) != ESP_OK) {
            if (s_last_act && now - s_last_act > 1000000) {
                portENTER_CRITICAL(&s_dc_mux);
                s_dc_tail = s_dc_head;
                portEXIT_CRITICAL(&s_dc_mux);
            }
            continue;
        }
        s_last_act = now;

        uint8_t dc = dc_pop();
        if (dc) g_stat.dc1_cnt++;
        else g_stat.dc0_cnt++;
        size_t bytes = out->trans_len / 8;
        if (bytes > TBN) bytes = TBN;
        if (out->trans_len && out->trans_len % 32) {
            g_stat.mis_bursts++;
            if (now - s_last_mis > 1000000) {
                s_last_mis = now;
                ESP_LOGW(TAG, "host burst %u bits is not a multiple of 32; slave DMA may drop tail bytes",
                         (unsigned)out->trans_len);
            }
        }
        if (bytes) {
            g_stat.spi_trans_cnt++;
            g_stat.spi_rx_bytes += (uint32_t)bytes;
            if (bytes == TBN) g_stat.spi_full_burst++;
            if (bytes > 1024) g_stat.big_bursts++;
            if (now - s_last_dump > 500000 && (dc == 0 || bytes > 1024)) {
                s_last_dump = now;
                char hx[64];
                size_t k = bytes < 16 ? bytes : 16;
                int o = 0;
                for (size_t j = 0; j < k; j++) {
                    o += snprintf(hx + o, sizeof(hx) - (size_t)o, "%02x ", ((uint8_t *)out->rx_buffer)[j]);
                    if (o >= (int)sizeof(hx) - 3) break;
                }
                ESP_LOGI(TAG, "dc=%u len=%u first: %s", dc, (unsigned)bytes, hx);
            }
            proto_parse(dc != 0, out->rx_buffer, bytes);
        }

        out->trans_len = 0;
        spi_slave_queue_trans(HOST, out, portMAX_DELAY);
    }
}

void spi_capture_start(void)
{
    for (int i = 0; i < QN; i++) {
        s_slots[i].buf = heap_caps_malloc(TBN, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!s_slots[i].buf) {
            ESP_LOGE(TAG, "rx buffer alloc failed");
            return;
        }
        s_slots[i].t.user = &s_slots[i];
    }

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << CONFIG_TFT_EMU_PIN_CS) | (1ULL << CONFIG_TFT_EMU_PIN_SCK) |
                        (1ULL << CONFIG_TFT_EMU_PIN_MOSI) | (1ULL << CONFIG_TFT_EMU_PIN_DC),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);

    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    if (CONFIG_TFT_EMU_PIN_RES >= 0) {
        gpio_config_t ri = {
            .pin_bit_mask = 1ULL << CONFIG_TFT_EMU_PIN_RES,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        gpio_config(&ri);
        gpio_isr_handler_add(CONFIG_TFT_EMU_PIN_RES, res_pin_cb, NULL);
    }
    gpio_set_intr_type(CONFIG_TFT_EMU_PIN_CS, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(CONFIG_TFT_EMU_PIN_CS, cs_fall_cb, NULL);

    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_TFT_EMU_PIN_MOSI,
        .miso_io_num = CONFIG_TFT_EMU_PIN_MISO,
        .sclk_io_num = CONFIG_TFT_EMU_PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TBN,
    };

    spi_dma_chan_t dma = SPI_DMA_DISABLED;
    bool want_dma = g_cfg.spi_dma == 1;
    if (want_dma && g_cfg.spi_mode != 1 && g_cfg.spi_mode != 3) {
        ESP_LOGW(TAG, "DMA not usable in SPI mode %u (HW requires mode 1/3) - using FIFO mode", g_cfg.spi_mode);
        want_dma = false;
    }
    if (want_dma) dma = SPI_DMA_CH_AUTO;

    spi_slave_interface_config_t slv = {
        .spics_io_num = CONFIG_TFT_EMU_PIN_CS,
        .queue_size = QN,
        .mode = g_cfg.spi_mode,
    };

    esp_err_t err = spi_slave_initialize(HOST, &bus, &slv, dma);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_slave_initialize failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "slave ready host=%u mode=%u dma=%u trans=%uB queue=%u", HOST, g_cfg.spi_mode,
             (int)(dma != SPI_DMA_DISABLED), TBN, QN);

    if (xTaskCreatePinnedToCore(capture_task, "spi_cap", 8192, NULL, 6, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "capture task start failed");
    }
}
