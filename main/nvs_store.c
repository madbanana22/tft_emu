#include "nvs_store.h"
#include "tft_emu.h"

#include <nvs_flash.h>
#include <nvs.h>
#include <string.h>
#include <esp_log.h>
#include <sdkconfig.h>

#define CFG_NS "tft_emu"
#define CFG_KEY "cfg"

tft_settings_t g_cfg;

static const char *TAG = "cfg";

static void settings_defaults(void)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.hver = TFT_EMU_HVER;
    g_cfg.ram_w = 80;
    g_cfg.ram_h = 160;
    g_cfg.off_x = 0;
    g_cfg.off_y = 0;
    g_cfg.rot = 0;
    g_cfg.pixel_swap = 0;
    g_cfg.addr_bytes = 4;
    g_cfg.madctl_style = 0;
    g_cfg.spi_mode = 1;
    g_cfg.spi_dma = 1;
    strncpy(g_cfg.ap_pass, CONFIG_TFT_EMU_AP_PASS, sizeof(g_cfg.ap_pass) - 1);
    g_cfg.pin_loopback = 0;
}

esp_err_t settings_save(void)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(CFG_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, CFG_KEY, &g_cfg, sizeof(g_cfg));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK)
        ESP_LOGI(TAG, "saved %ux%u off=%u,%u rot=%u pswap=%u ab=%u style=%u spi=%u dma=%u lb=%u",
                 g_cfg.ram_w, g_cfg.ram_h, g_cfg.off_x, g_cfg.off_y, g_cfg.rot,
                 g_cfg.pixel_swap, g_cfg.addr_bytes, g_cfg.madctl_style, g_cfg.spi_mode,
                 g_cfg.spi_dma, g_cfg.pin_loopback);
    return e;
}

esp_err_t settings_load(void)
{
    settings_defaults();
    nvs_handle_t h;
    esp_err_t e = nvs_open(CFG_NS, NVS_READONLY, &h);
    if (e == ESP_OK) {
        size_t len = sizeof(g_cfg);
        e = nvs_get_blob(h, CFG_KEY, &g_cfg, &len);
        nvs_close(h);
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no stored settings (using defaults)");
        memset(g_cfg.sta_ssid, 0, sizeof(g_cfg.sta_ssid));
        memset(g_cfg.sta_pass, 0, sizeof(g_cfg.sta_pass));
        memset(g_cfg.ap_ssid, 0, sizeof(g_cfg.ap_ssid));
        memset(g_cfg.ap_pass, 0, sizeof(g_cfg.ap_pass));
        strncpy(g_cfg.ap_pass, CONFIG_TFT_EMU_AP_PASS, sizeof(g_cfg.ap_pass) - 1);
        return ESP_OK;
    }
    g_cfg.ram_w = g_cfg.ram_w ? g_cfg.ram_w : 80;
    g_cfg.ram_h = g_cfg.ram_h ? g_cfg.ram_h : 160;
    if (g_cfg.ram_w > TFT_MAX_RAM_COLS) g_cfg.ram_w = TFT_MAX_RAM_COLS;
    if (g_cfg.ram_h > TFT_MAX_RAM_ROWS) g_cfg.ram_h = TFT_MAX_RAM_ROWS;
    if (g_cfg.ram_w * g_cfg.ram_h * 2 > TFT_MAX_RAM_BYTES) g_cfg.ram_w = 80, g_cfg.ram_h = 160;
    if (g_cfg.spi_mode > 3) g_cfg.spi_mode = 3;
    g_cfg.addr_bytes = (g_cfg.addr_bytes == 2 || g_cfg.addr_bytes == 4) ? g_cfg.addr_bytes : 4;
    g_cfg.madctl_style = g_cfg.madctl_style ? 1 : 0;
    g_cfg.pixel_swap = g_cfg.pixel_swap ? 1 : 0;
    g_cfg.spi_dma = g_cfg.spi_dma ? 1 : 0;
    g_cfg.sta_ssid[sizeof(g_cfg.sta_ssid) - 1] = 0;
    g_cfg.sta_pass[sizeof(g_cfg.sta_pass) - 1] = 0;
    g_cfg.ap_ssid[sizeof(g_cfg.ap_ssid) - 1] = 0;
    g_cfg.ap_pass[sizeof(g_cfg.ap_pass) - 1] = 0;
    g_cfg.pin_loopback = g_cfg.pin_loopback ? 1 : 0;
    if (g_cfg.hver != TFT_EMU_HVER) {
        ESP_LOGW(TAG, "settings version mismatch (%u -> %u), ignoring wifi keys", g_cfg.hver, TFT_EMU_HVER);
        g_cfg.hver = TFT_EMU_HVER;
        memset(g_cfg.sta_ssid, 0, sizeof(g_cfg.sta_ssid));
        memset(g_cfg.sta_pass, 0, sizeof(g_cfg.sta_pass));
    }
    ESP_LOGI(TAG, "loaded %ux%u ssid=%s", g_cfg.ram_w, g_cfg.ram_h, g_cfg.sta_ssid[0] ? g_cfg.sta_ssid : "-");
    return ESP_OK;
}

esp_err_t settings_reset(void)
{
    settings_defaults();
    esp_err_t e = nvs_flash_erase();
    ESP_LOGW(TAG, "settings reset to defaults (%s)", esp_err_to_name(e));
    return e;
}

bool nvs_force_ap_get(void)
{
    nvs_handle_t h;
    bool ret = false;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t flag = 0;
        if (nvs_get_u8(h, "fap", &flag) == ESP_OK && flag) {
            ret = true;
            nvs_set_u8(h, "fap", 0);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    return ret;
}

void nvs_force_ap_set(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fap", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

tft_stats_t g_stat;
