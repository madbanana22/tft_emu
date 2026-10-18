#include "test_gen.h"
#include "tft_emu.h"
#include "panel_fb.h"
#include "spi_capture.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>
#include <sdkconfig.h>

#define TEST_HOST ((spi_host_device_t)CONFIG_TFT_EMU_TEST_SPI_HOST)
#define PIN_SCK   CONFIG_TFT_EMU_TEST_PIN_SCK
#define PIN_MOSI  CONFIG_TFT_EMU_TEST_PIN_MOSI
#define PIN_CS    CONFIG_TFT_EMU_TEST_PIN_CS
#define PIN_DC    CONFIG_TFT_EMU_TEST_PIN_DC
#define PIN_RES   CONFIG_TFT_EMU_TEST_PIN_RES

#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

static const char *TAG = "test";

static spi_device_handle_t s_dev;
static int s_dev_mhz;
static TaskHandle_t s_task;
static volatile bool s_run;
static volatile int s_pattern;
static volatile int s_fps = 10;
static int s_mhz = CONFIG_TFT_EMU_TEST_MHZ;
static uint8_t *s_frame;
static size_t s_need4;
static uint16_t s_w, s_h;
static uint32_t s_frame_no;
static uint8_t s_cmd[16];

static const char *s_names[TFT_TEST_PATTERN_COUNT] = {
    "rainbow", "progress bar", "color bars", "checkerboard", "solid sweep", "gradient"
};

const char *test_gen_pattern_name(int p)
{
    if (p < 0 || p >= TFT_TEST_PATTERN_COUNT) return "?";
    return s_names[p];
}

bool test_gen_running(void) { return s_task != NULL; }
int test_gen_pattern(void) { return s_pattern; }
int test_gen_fps(void) { return s_fps; }
int test_gen_mhz(void) { return s_mhz; }

static uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v)
{
    uint8_t region = (uint8_t)(h / 43);
    uint8_t rem = (uint8_t)((h - region * 43) * 6);
    uint8_t p = (uint8_t)((v * (255 - s)) >> 8);
    uint8_t q = (uint8_t)((v * (255 - ((s * rem) >> 8))) >> 8);
    uint8_t t = (uint8_t)((v * (255 - ((s * (255 - rem)) >> 8))) >> 8);
    uint8_t r, g, b;
    switch (region) {
    case 0:  r = v; g = t; b = p; break;
    case 1:  r = q; g = v; b = p; break;
    case 2:  r = p; g = v; b = t; break;
    case 3:  r = p; g = q; b = v; break;
    case 4:  r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGB565(r, g, b);
}

static bool s_bus_init;
static bool s_bus_loopback;
static int s_dc_pin;
static int s_res_pin;

static esp_err_t bus_teardown(void)
{
    if (s_dev) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
        s_dev_mhz = 0;
    }
    if (s_bus_init) {
        spi_bus_free(TEST_HOST);
        s_bus_init = false;
    }
    s_bus_loopback = false;
    return ESP_OK;
}

static esp_err_t ensure_master(int mhz)
{
    if (TEST_HOST == (spi_host_device_t)CONFIG_TFT_EMU_SPI_HOST) {
        ESP_LOGE(TAG, "test master host == emulator slave host (%d)", (int)TEST_HOST);
        return ESP_ERR_INVALID_ARG;
    }
    bool lb = g_cfg.pin_loopback != 0;
    s_dc_pin = lb ? CONFIG_TFT_EMU_PIN_DC : PIN_DC;
    s_res_pin = lb ? CONFIG_TFT_EMU_PIN_RES : PIN_RES;
    if (lb) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << CONFIG_TFT_EMU_PIN_DC),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
#if CONFIG_TFT_EMU_PIN_RES >= 0
        io.pin_bit_mask |= (1ULL << CONFIG_TFT_EMU_PIN_RES);
#endif
        gpio_config(&io);
        gpio_set_level(CONFIG_TFT_EMU_PIN_DC, 1);
#if CONFIG_TFT_EMU_PIN_RES >= 0
        gpio_set_level(CONFIG_TFT_EMU_PIN_RES, 1);
        gpio_set_intr_type(CONFIG_TFT_EMU_PIN_RES, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(CONFIG_TFT_EMU_PIN_RES);
#endif
    }
    if (s_bus_init && s_bus_loopback != lb) bus_teardown();
    if (!s_bus_init) {
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << (unsigned)s_dc_pin),
            .mode = GPIO_MODE_INPUT_OUTPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        if (!lb && s_res_pin >= 0) {
            io.pin_bit_mask |= (1ULL << (unsigned)s_res_pin);
            io.mode = GPIO_MODE_OUTPUT;
            io.pull_up_en = GPIO_PULLUP_DISABLE;
        }
        gpio_config(&io);
        gpio_set_level(s_dc_pin, 1);
#if CONFIG_TFT_EMU_PIN_RES >= 0
        gpio_set_intr_type(CONFIG_TFT_EMU_PIN_RES, GPIO_INTR_NEGEDGE);
        gpio_intr_enable(CONFIG_TFT_EMU_PIN_RES);
#endif

        spi_bus_config_t bus = {
            .mosi_io_num = lb ? CONFIG_TFT_EMU_PIN_MOSI : PIN_MOSI,
            .miso_io_num = -1,
            .sclk_io_num = lb ? CONFIG_TFT_EMU_PIN_SCK : PIN_SCK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 65536,
        };
        esp_err_t e = spi_bus_initialize(TEST_HOST, &bus, SPI_DMA_CH_AUTO);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "test master bus init failed: %s", esp_err_to_name(e));
            return e;
        }
        s_bus_init = true;
        s_bus_loopback = lb;
    }
    if (s_dev) {
        if (s_dev_mhz == mhz && s_bus_loopback == lb) return ESP_OK;
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
        s_dev_mhz = 0;
    }

    spi_device_interface_config_t dev = {
        .clock_speed_hz = mhz * 1000000,
        .mode = g_cfg.spi_mode,
        .spics_io_num = lb ? CONFIG_TFT_EMU_PIN_CS : PIN_CS,
        .queue_size = 1,
    };
    esp_err_t e = spi_bus_add_device(TEST_HOST, &dev, &s_dev);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "test master add device failed: %s", esp_err_to_name(e));
        return e;
    }
    s_dev_mhz = mhz;
    return ESP_OK;
}

static esp_err_t send_block(const uint8_t *b, size_t n, int dc)
{
    gpio_set_level(s_dc_pin, dc);
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = n * 8;
    t.tx_buffer = b;
    return spi_device_transmit(s_dev, &t);
}

static esp_err_t send_window(uint16_t w, uint16_t h)
{
    uint8_t *c = s_cmd;
    int i = 0;
    c[i++] = 0x2A;
    c[i++] = 0x00;
    c[i++] = 0x00;
    c[i++] = (uint8_t)((w - 1) >> 8);
    c[i++] = (uint8_t)((w - 1) & 0xFF);
    c[i++] = 0x2B;
    c[i++] = 0x00;
    c[i++] = 0x00;
    c[i++] = (uint8_t)((h - 1) >> 8);
    c[i++] = (uint8_t)((h - 1) & 0xFF);
    c[i++] = 0x2C;
    while (i & 3) c[i++] = 0x00;
    return send_block(c, (size_t)i, 0);
}

static esp_err_t send_pixels(const uint8_t *fb, size_t n)
{
    size_t chunk = CONFIG_TFT_EMU_TRANS_BYTES;
    while (n) {
        size_t take = (n < chunk) ? n : chunk;
        esp_err_t e = send_block(fb, take, 1);
        if (e != ESP_OK) return e;
        fb += take;
        n -= take;
    }
    return ESP_OK;
}

static void render(uint16_t w, uint16_t h, uint32_t f)
{
    bool swap = g_cfg.pixel_swap != 0;
    size_t o = 0;
    for (uint16_t y = 0; y < h; y++) {
        for (uint16_t x = 0; x < w; x++) {
            uint16_t c;
            switch (s_pattern) {
            case TFT_TEST_RAINBOW: {
                uint8_t hue = (uint8_t)((uint32_t)x * 255 / w + f * 4);
                uint8_t val = (uint8_t)(255 - (uint32_t)y * 140 / h);
                c = hsv565(hue, 255, val);
            } break;
            case TFT_TEST_PROGRESS: {
                uint32_t pos = (f * 2) % (w + 20);
                uint32_t fill = pos > w ? w : pos;
                if (x < fill) {
                    c = ((uint32_t)x + 3 >= fill) ? RGB565(255, 255, 255) : RGB565(50, 190, 90);
                } else {
                    c = RGB565(18, 18, 22);
                }
            } break;
            case TFT_TEST_BARS: {
                static const uint16_t bars[8] = {
                    RGB565(255, 255, 255), RGB565(255, 255, 0), RGB565(0, 255, 255), RGB565(0, 255, 0),
                    RGB565(255, 0, 255),   RGB565(255, 0, 0),   RGB565(0, 0, 255),   RGB565(0, 0, 0)
                };
                uint32_t bw = w / 8 ? w / 8 : 1;
                uint32_t idx = x / bw;
                if (idx > 7) idx = 7;
                c = bars[idx];
                if (y == (uint16_t)((f * 4) % h)) c = RGB565(0, 0, 0);
            } break;
            case TFT_TEST_CHECKER: {
                uint32_t a = ((uint32_t)(x + f * 2) / 16) + (y / 16);
                c = (a & 1) ? RGB565(235, 235, 235) : RGB565(25, 25, 80);
            } break;
            case TFT_TEST_SOLID: {
                c = hsv565((uint8_t)(f * 3), 255, 255);
            } break;
            default: {
                uint8_t r = (uint8_t)((uint32_t)x * 255 / w);
                uint8_t g = (uint8_t)((uint32_t)y * 255 / h);
                uint8_t b = (uint8_t)(f * 3);
                c = RGB565(r, g, b);
            } break;
            }
            if (swap) {
                s_frame[o++] = (uint8_t)c;
                s_frame[o++] = (uint8_t)(c >> 8);
            } else {
                s_frame[o++] = (uint8_t)(c >> 8);
                s_frame[o++] = (uint8_t)c;
            }
        }
    }
}

static void test_task(void *arg)
{
    (void)arg;

    if (s_res_pin >= 0) {
        gpio_set_level(s_res_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(s_res_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    TickType_t next = xTaskGetTickCount();
    while (s_run) {
        uint16_t w = panel_w();
        uint16_t h = panel_h();
        if (!w || !h) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t need = (size_t)w * h * 2;
        size_t need4 = (need + 3u) & ~3u;
        if (!s_frame || need4 != s_need4) {
            uint8_t *nb = heap_caps_malloc(need4, MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
            if (!nb) {
                ESP_LOGE(TAG, "pattern buffer alloc failed (%u bytes)", (unsigned)need4);
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }
            if (s_frame) free(s_frame);
            s_frame = nb;
            s_need4 = need4;
            s_w = w;
            s_h = h;
        }
        render(s_w, s_h, s_frame_no);
        for (size_t o = need; o < need4; o += 2) {
            s_frame[o] = s_frame[0];
            s_frame[o + 1] = s_frame[1];
        }
        if (send_window(s_w, s_h) != ESP_OK) break;
        if (send_pixels(s_frame, need4) != ESP_OK) break;
        s_frame_no++;
        int fps = s_fps > 0 ? s_fps : 1;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000 / fps));
    }

    if (s_frame) {
        free(s_frame);
        s_frame = NULL;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t test_gen_start(int pattern, int fps, int mhz)
{
    if (pattern < 0 || pattern >= TFT_TEST_PATTERN_COUNT) pattern = 0;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    if (mhz < 1) mhz = 1;
    if (mhz > 40) mhz = 40;

    if (s_task) {
        s_pattern = pattern;
        s_fps = fps;
        if (mhz == s_mhz && g_cfg.pin_loopback == s_bus_loopback) return ESP_OK;
        test_gen_stop();
    }

    if (!panel_valid()) {
        ESP_LOGE(TAG, "no framebuffer - configure a display size first");
        return ESP_ERR_INVALID_STATE;
    }
    s_w = panel_w();
    s_h = panel_h();
    if (!s_w || !s_h) return ESP_ERR_INVALID_STATE;

    if (g_cfg.spi_mode != 1 && g_cfg.spi_mode != 3) {
        ESP_LOGW(TAG, "emulator SPI mode %u cannot use DMA; set mode 1 or 3 for reliable test capture",
                 g_cfg.spi_mode);
    }

    esp_err_t e = ensure_master(mhz);
    if (e != ESP_OK) return e;

    s_pattern = pattern;
    s_fps = fps;
    s_mhz = mhz;
    s_frame_no = 0;
    s_run = true;
    if (xTaskCreatePinnedToCore(test_task, "test_gen", 4096, NULL, 3, &s_task, 1) != pdPASS) {
        s_run = false;
        s_task = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "test '%s' %ux%u @ %d fps, %d MHz, mode %u, pins %s",
             test_gen_pattern_name(pattern), panel_w(), panel_h(), fps, mhz, g_cfg.spi_mode,
             g_cfg.pin_loopback ? "internal loopback" : "jumpered to emulator");
    return ESP_OK;
}

esp_err_t test_gen_stop(void)
{
    s_run = false;
    for (int i = 0; i < 400 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    if (!s_task && s_frame) {
        free(s_frame);
        s_frame = NULL;
    }
    if (s_bus_init) {
        bus_teardown();
        spi_capture_pins_rest();
    }
    ESP_LOGI(TAG, "test stopped");
    return ESP_OK;
}
