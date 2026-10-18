#include "tft_emu.h"
#include "nvs_store.h"
#include "panel_fb.h"
#include "spi_capture.h"
#include "proto_st7735.h"
#include "wifi_mgr.h"
#include "web_ui.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <esp_log.h>
#include <sdkconfig.h>
#include <nvs_flash.h>
#include <stdio.h>

static const char *TAG = "main";

const char *tft_ver(void) { return "tft-emu v1.0"; }

static void boot_gpio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << CONFIG_TFT_EMU_PIN_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io);
}

static void boot_button_task(void *arg)
{
    int held = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (wifi_get_mode() != WIFI_MODE_STA_EMU) {
            held = 0;
            continue;
        }
        if (gpio_get_level(CONFIG_TFT_EMU_PIN_BOOT) == 0) {
            held++;
            if (held >= 10) {
                ESP_LOGW(TAG, "BOOT button: switching to AP mode for next boot");
                nvs_force_ap_set(true);
                esp_restart();
            }
        } else {
            held = 0;
        }
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    settings_load();
    boot_gpio_init();
    ESP_ERROR_CHECK(panel_init());
    proto_reset();
    spi_capture_start();
    wifi_boot();

    xTaskCreate(boot_button_task, "btn", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "%s ready (%ux%u, spi mode %u dma %u, waiting for host)", tft_ver(), g_cfg.ram_w,
             g_cfg.ram_h, g_cfg.spi_mode, g_cfg.spi_dma);

    uint32_t last_bytes = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(TAG, "%ds: rx=%luKB trans=%lu cmds=%lu frames=%lu heap=%u", (int)(xTaskGetTickCount() / 1000),
                 (unsigned long)((g_stat.spi_rx_bytes - last_bytes) / 1024),
                 (unsigned long)g_stat.spi_trans_cnt, (unsigned long)g_stat.proto_cmd_cnt,
                 (unsigned long)g_stat.mem_tx_frames, (unsigned int)esp_get_free_heap_size());
        last_bytes = g_stat.spi_rx_bytes;
    }
}
