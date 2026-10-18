#include "wifi_mgr.h"
#include "tft_emu.h"
#include "nvs_store.h"
#include "web_ui.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <esp_mac.h>
#include <esp_log.h>
#include <string.h>
#include <stdio.h>
#include <sdkconfig.h>
#include <driver/gpio.h>

#define WIFI_FAIL_BIT BIT1
#define WIFI_STA_MAX_RETRY 3
#define WIFI_STA_TIMEOUT_S 25

static const char *TAG = "wifi";

static emu_wifi_mode_t s_mode = WIFI_MODE_NONE_EMU;
static char s_ip[16] = "";
static int s_retry;
static EventGroupHandle_t s_eg;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_STA_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_retry, WIFI_STA_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "cannot reach network after %d tries", WIFI_STA_MAX_RETRY);
            xEventGroupSetBits(s_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "got ip %s", s_ip);
        xEventGroupSetBits(s_eg, BIT0);
        web_ui_start();
    }
}

void wifi_default_ap_name(char *dst, size_t cap)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(dst, cap, "TFT-EMU-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

emu_wifi_mode_t wifi_get_mode(void) { return s_mode; }
const char *wifi_get_ip(void) { return s_ip; }
const char *wifi_get_ssid(void) { return s_mode == WIFI_MODE_STA_EMU ? g_cfg.sta_ssid : NULL; }

static void start_ap(void)
{
    s_mode = WIFI_MODE_AP_EMU;
    char name[33] = "";
    const char *ssid = g_cfg.ap_ssid[0] ? g_cfg.ap_ssid : (wifi_default_ap_name(name, sizeof(name)), name);
    const char *pass = g_cfg.ap_pass[0] ? g_cfg.ap_pass : CONFIG_TFT_EMU_AP_PASS;

    wifi_ap_config_t ap = { 0 };
    ap.channel = 6;
    ap.max_connection = 4;
    ap.ssid_len = (uint8_t)strlen(ssid);
    memcpy(ap.ssid, ssid, ap.ssid_len);
    size_t plen = strlen(pass);
    if (plen > sizeof(ap.password) - 1) plen = sizeof(ap.password) - 1;
    memcpy(ap.password, pass, plen);
    ap.password[plen] = 0;
    wifi_band_mode_t band = WIFI_BAND_MODE_2G_ONLY;
    ap.authmode = strlen(pass) >= 8 ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config_t wc = { 0 };
    memcpy(&wc.ap, &ap, sizeof(ap));
    esp_err_t e1 = esp_wifi_set_mode(WIFI_MODE_AP);
    esp_err_t e2 = esp_wifi_set_config(WIFI_IF_AP, &wc);
    esp_err_t e3 = esp_wifi_set_band_mode(band);
    esp_err_t e4 = esp_wifi_start();
    ESP_LOGI(TAG, "AP up err=%s %s %s %s ssid=%s pass=%s", esp_err_to_name(e1), esp_err_to_name(e2),
             esp_err_to_name(e3), esp_err_to_name(e4), ssid, ap.authmode ? "***" : "open");
    snprintf(s_ip, sizeof(s_ip), "192.168.4.1");
    web_ui_start();
}

static void start_sta(void)
{
    s_mode = WIFI_MODE_STA_EMU;
    wifi_config_t cfg = { 0 };
    size_t ssid_len = strlen(g_cfg.sta_ssid);
    size_t pass_len = strlen(g_cfg.sta_pass);
    if (ssid_len > sizeof(cfg.sta.ssid) - 1) ssid_len = sizeof(cfg.sta.ssid) - 1;
    if (pass_len > sizeof(cfg.sta.password) - 1) pass_len = sizeof(cfg.sta.password) - 1;
    memcpy(cfg.sta.ssid, g_cfg.sta_ssid, ssid_len);
    memcpy(cfg.sta.password, g_cfg.sta_pass, pass_len);
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_STA);
    e = e ?: esp_wifi_set_config(WIFI_IF_STA, &cfg);
    e = e ?: esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "wifi sta start failed: %s", esp_err_to_name(e));
        esp_wifi_deinit();
        s_mode = WIFI_MODE_NONE_EMU;
        start_ap();
        return;
    }
    ESP_LOGI(TAG, "STA connecting to '%s'...", g_cfg.sta_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_eg, BIT0 | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_STA_TIMEOUT_S * 1000));
    if (bits & BIT0) return;
    if (!(bits & WIFI_FAIL_BIT)) {
        ESP_LOGW(TAG, "STA connect timeout");
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    s_mode = WIFI_MODE_NONE_EMU;
    s_ip[0] = 0;
    start_ap();
}

void wifi_boot(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));
    s_eg = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wic));

    esp_err_t storage = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    (void)storage;

    bool force_ap = nvs_force_ap_get();
    force_ap |= gpio_get_level(CONFIG_TFT_EMU_PIN_BOOT) == 0;
    s_retry = 0;

    if (g_cfg.sta_ssid[0] && !force_ap) start_sta();
    else start_ap();
}
