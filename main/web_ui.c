#include "web_ui.h"
#include "tft_emu.h"
#include "nvs_store.h"
#include "panel_fb.h"
#include "wifi_mgr.h"
#include "test_gen.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <proto_st7735.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_WS_CLIENTS 4
#define WS_HDR_SIZE 16
#define TILE_PAYLOAD (PANEL_TILE * PANEL_TILE * 2)
#define MAX_TILES (TFT_MAX_RAM_COLS / PANEL_TILE + 1) * (TFT_MAX_RAM_ROWS / PANEL_TILE + 1)

static const char *TAG = "web";

extern const uint8_t html_bin_start[] asm("_binary_index_html_start");
extern const uint8_t html_bin_end[] asm("_binary_index_html_end");

static httpd_handle_t s_http;
static SemaphoreHandle_t s_cli_mux;
static int s_clients[MAX_WS_CLIENTS];
static volatile int s_client_cnt;
static bool s_started;

static uint8_t s_stage[4096];
static uint32_t s_frame_id;
static int s_prev_client_cnt;
static uint16_t s_sent_w, s_sent_h;

static esp_err_t ws_send_frag(httpd_handle_t handle, int fd, const uint8_t *data, size_t len, bool first, bool final)
{
    httpd_ws_frame_t p;
    memset(&p, 0, sizeof(p));
    p.fragmented = !final;
    p.final = final;
    p.type = first ? HTTPD_WS_TYPE_BINARY : HTTPD_WS_TYPE_CONTINUE;
    p.payload = (uint8_t *)data;
    p.len = len;
    return httpd_ws_send_data(handle, fd, &p);
}

static void client_add(int fd)
{
    xSemaphoreTake(s_cli_mux, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            xSemaphoreGive(s_cli_mux);
            return;
        }
    }
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i] < 0) {
            s_clients[i] = fd;
            s_client_cnt++;
            break;
        }
    }
    xSemaphoreGive(s_cli_mux);
}

static void client_del(int fd)
{
    xSemaphoreTake(s_cli_mux, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_clients[i] == fd) {
            s_clients[i] = -1;
            s_client_cnt--;
            break;
        }
    }
    xSemaphoreGive(s_cli_mux);
}

static void client_snapshot(int *out)
{
    xSemaphoreTake(s_cli_mux, portMAX_DELAY);
    memcpy(out, s_clients, sizeof(s_clients));
    xSemaphoreGive(s_cli_mux);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        client_add(fd);
        return ESP_OK;
    }
    httpd_ws_frame_t fr;
    memset(&fr, 0, sizeof(fr));
    if (httpd_ws_recv_frame(req, &fr, 0) != ESP_OK) {
        return ESP_OK;
    }
    uint8_t buf[32];
    if (fr.len) {
        if (fr.len > sizeof(buf)) fr.len = sizeof(buf);
        fr.payload = buf;
        if (httpd_ws_recv_frame(req, &fr, fr.len) != ESP_OK) {
            return ESP_OK;
        }
    }
    if (fr.type == HTTPD_WS_TYPE_TEXT || fr.type == HTTPD_WS_TYPE_PONG || fr.type == HTTPD_WS_TYPE_PING) {
        client_add(fd);
    } else if (fr.type == HTTPD_WS_TYPE_CLOSE) {
        client_del(fd);
    }
    return ESP_OK;
}

static void stream_task(void *arg)
{
    (void)arg;
    int snap[MAX_WS_CLIENTS];
    uint8_t hdr[WS_HDR_SIZE];
    TickType_t next = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(1000 / CONFIG_TFT_EMU_WS_FPS);

    for (;;) {
        vTaskDelayUntil(&next, period);
        int cnt = s_client_cnt;
        if (cnt == 0 || !panel_valid()) continue;

        uint16_t w = panel_w();
        uint16_t h = panel_h();
        uint16_t tw = (uint16_t)((w + PANEL_TILE - 1) / PANEL_TILE);
        uint32_t total_tiles = (uint32_t)tw * ((h + PANEL_TILE - 1) / PANEL_TILE);
        uint32_t nd = panel_dirty_count();

        bool full = (cnt != s_prev_client_cnt) || (nd * 5 >= total_tiles * 2) ||
                    (w != s_sent_w) || (h != s_sent_h);

        uint16_t tile_list[MAX_TILES];
        uint16_t ntiles = 0;
        if (!full) {
            ntiles = panel_dirty_tiles(tile_list, (uint16_t)(sizeof(tile_list) / sizeof(tile_list[0])));
            if (ntiles == 0) {
                s_prev_client_cnt = cnt;
                continue;
            }
        }

        size_t payload = full ? ((size_t)w * h * 2) : (2u + (size_t)ntiles * (2u + TILE_PAYLOAD));
        s_frame_id++;

        hdr[0] = 0xA5;
        hdr[1] = 0x4E;
        hdr[2] = full ? 1 : 2;
        hdr[3] = panel_madctl();
        hdr[4] = (uint8_t)(g_cfg.madctl_style ? 1 : 0);
        hdr[5] = (uint8_t)(g_cfg.pixel_swap ? 1 : 0);
        hdr[6] = (uint8_t)(w & 0xff);
        hdr[7] = (uint8_t)(w >> 8);
        hdr[8] = (uint8_t)(h & 0xff);
        hdr[9] = (uint8_t)(h >> 8);
        hdr[10] = (uint8_t)(s_frame_id & 0xff);
        hdr[11] = (uint8_t)((s_frame_id >> 8) & 0xff);
        hdr[12] = (uint8_t)((s_frame_id >> 16) & 0xff);
        hdr[13] = (uint8_t)((s_frame_id >> 24) & 0xff);
        hdr[14] = (uint8_t)(payload & 0xff);
        hdr[15] = (uint8_t)((payload >> 8) & 0xff);

        client_snapshot(snap);
        int sent = 0;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            int fd = snap[i];
            if (fd < 0) continue;
            esp_err_t err = ws_send_frag(s_http, fd, hdr, WS_HDR_SIZE, true, false);
            if (err == ESP_OK && full) {
                size_t total = (size_t)w * h * 2;
                size_t off = 0;
                while (off < total && err == ESP_OK) {
                    size_t take = (total - off > sizeof(s_stage)) ? sizeof(s_stage) : (total - off);
                    panel_copy_span(off, s_stage, take);
                    off += take;
                    err = ws_send_frag(s_http, fd, s_stage, take, false, off >= total);
                }
                panel_clear_dirty();
            } else if (err == ESP_OK) {
                uint8_t cntb[2] = { (uint8_t)(ntiles & 0xff), (uint8_t)(ntiles >> 8) };
                err = ws_send_frag(s_http, fd, cntb, 2, false, false);
                for (uint16_t k = 0; k < ntiles && err == ESP_OK; k++) {
                    uint16_t t = tile_list[k];
                    s_stage[0] = (uint8_t)(t & 0xff);
                    s_stage[1] = (uint8_t)(t >> 8);
                    panel_copy_tile(t, s_stage + 2);
                    err = ws_send_frag(s_http, fd, s_stage, 2 + TILE_PAYLOAD, false, (k + 1) == ntiles);
                }
            }
            if (err != ESP_OK) {
                client_del(fd);
            } else {
                sent++;
            }
        }
        g_stat.mem_tx_frames += (uint32_t)sent;
        g_stat.mem_tx_bytes += (uint32_t)(payload * (size_t)sent);
        s_prev_client_cnt = cnt;
        s_sent_w = w;
        s_sent_h = h;
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    size_t len = (size_t)(html_bin_end - html_bin_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_send(req, (const char *)html_bin_start, len);
    return ESP_OK;
}

static int json_body(httpd_req_t *req, char *buf, size_t bufsz)
{
    size_t got = 0;
    size_t want = bufsz - 1;
    while (got < want) {
        int r = httpd_req_recv(req, buf + got, want - got);
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) break;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    buf[got] = 0;
    return (int)got;
}

static bool jfind(char *j, const char *key, char *out, size_t outsz)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    char *p = j;
    while ((p = strstr(p, pat)) != NULL) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p != ':') continue;
        p++;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '"') {
            p++;
            size_t n = 0;
            while (*p && *p != '"') {
                if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '/' ||
                                   (p[1] >= 'b' && p[1] <= 'u'))) {
                    p++;
                    if (*p == 'n') *p = '\n';
                    else if (*p == 'r') *p = '\r';
                    else if (*p == 't') *p = '\t';
                }
                if (n + 1 < outsz) out[n++] = *p;
                p++;
            }
            out[n] = 0;
            return true;
        }
        char tmp[32];
        size_t n = 0;
        while (*p == '-' || *p == '+' || (*p >= '0' && *p <= '9') || *p == '.') {
            if (n + 1 < sizeof(tmp)) tmp[n++] = *p;
            p++;
        }
        tmp[n] = 0;
        int v = atoi(tmp);
        snprintf(out, outsz, "%d", v);
        return true;
    }
    return false;
}

static int jint(char *j, const char *key, int def)
{
    char tmp[32];
    if (!jfind(j, key, tmp, sizeof(tmp))) return def;
    return atoi(tmp);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void resp_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static int64_t last_us;
    static uint32_t last_spi_b, last_ws_b;
    char body[960];
    int64_t now = esp_timer_get_time();
    int64_t dt = last_us ? (now - last_us) : 0;
    uint32_t spi_b = g_stat.spi_rx_bytes - last_spi_b;
    uint32_t ws_b = g_stat.mem_tx_bytes - last_ws_b;
    uint32_t spi_kbps = (dt > 0) ? (uint32_t)((uint64_t)spi_b * 1000000ULL / (uint64_t)dt / 1000) : 0;
    uint32_t ws_kbps = (dt > 0) ? (uint32_t)((uint64_t)ws_b * 1000000ULL / (uint64_t)dt / 1000) : 0;
    last_spi_b = g_stat.spi_rx_bytes;
    last_ws_b = g_stat.mem_tx_bytes;
    last_us = now;

    const char *mode = wifi_get_mode() == WIFI_MODE_STA_EMU ? "sta" : "ap";
    snprintf(body, sizeof(body),
             "{"
             "\"ver\":\"%s\",\"mode\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\","
             "\"ram_w\":%u,\"ram_h\":%u,\"rot\":%u,\"pixel_swap\":%u,\"addr_bytes\":%u,"
              "\"madctl_style\":%u,\"spi_mode\":%u,\"spi_dma\":%u,\"loopback\":%u,\"mis\":%lu,"
             "\"test_enabled\":%u,\"test_pattern\":%u,\"test_fps\":%u,\"test_mhz\":%u,"
             "\"ws_clients\":%d,"
             "\"inj_cnt\":%lu,\"inj_bytes\":%lu,"
             "\"spi_kbps\":%lu,\"spi_trans\":%lu,\"spi_full\":%lu,\"cmd_cnt\":%lu,"
             "\"edges\":%lu,\"dc0\":%lu,\"dc1\":%lu,\"big\":%lu,"
             "\"ramwr\":%lu,\"pix\":%lu,"
             "\"frame_id\":%lu,\"ws_frames\":%lu,\"ws_kbps\":%lu,"
             "\"heap\":%u,\"up_s\":%lld}",
             tft_ver(), mode, wifi_get_ip(), wifi_get_ssid() ? wifi_get_ssid() : "",
             (unsigned)g_cfg.ram_w, (unsigned)g_cfg.ram_h, (unsigned)g_cfg.rot,
             (unsigned)g_cfg.pixel_swap, (unsigned)g_cfg.addr_bytes,
              (unsigned)g_cfg.madctl_style, (unsigned)g_cfg.spi_mode, (unsigned)g_cfg.spi_dma,
              (unsigned)g_cfg.pin_loopback, (unsigned long)g_stat.mis_bursts,
             (unsigned)(test_gen_running() ? 1 : 0), (unsigned)test_gen_pattern(),
             (unsigned)test_gen_fps(), (unsigned)test_gen_mhz(),
             s_client_cnt,
             (unsigned long)g_stat.inj_frames, (unsigned long)g_stat.inj_bytes,
             (unsigned long)spi_kbps, (unsigned long)g_stat.spi_trans_cnt,
             (unsigned long)g_stat.spi_full_burst, (unsigned long)g_stat.proto_cmd_cnt,
             (unsigned long)g_stat.cs_edges, (unsigned long)g_stat.dc0_cnt,
             (unsigned long)g_stat.dc1_cnt, (unsigned long)g_stat.big_bursts,
             (unsigned long)g_stat.ramwr_cnt, (unsigned long)g_stat.pix_cnt,
             (unsigned long)s_frame_id, (unsigned long)g_stat.mem_tx_frames,
             (unsigned long)ws_kbps,
             (unsigned int)esp_get_free_heap_size(), (long long)(now / 1000000));
    resp_json(req, body);
    return ESP_OK;
}

static esp_err_t display_handler(httpd_req_t *req)
{
    char body[1024];
    if (json_body(req, body, sizeof(body)) < 0) {
        resp_json(req, "{\"ok\":false}");
        return ESP_OK;
    }
    int w = jint(body, "ram_w", g_cfg.ram_w);
    int h = jint(body, "ram_h", g_cfg.ram_h);
    int rot = jint(body, "rot", g_cfg.rot);
    int pswap = jint(body, "pixel_swap", g_cfg.pixel_swap);
    int ab = jint(body, "addr_bytes", g_cfg.addr_bytes);
    int style = jint(body, "madctl_style", g_cfg.madctl_style);
    int smode = jint(body, "spi_mode", g_cfg.spi_mode);
    if (w < 16 || h < 16 || w > TFT_MAX_RAM_COLS || h > TFT_MAX_RAM_ROWS ||
        (int64_t)w * h * 2 > (int64_t)TFT_MAX_RAM_BYTES) {
        resp_json(req, "{\"ok\":false,\"msg\":\"bad size\"}");
        return ESP_OK;
    }
    bool size_changed = (w != (int)g_cfg.ram_w) || (h != (int)g_cfg.ram_h);
    bool mode_changed = smode != (int)g_cfg.spi_mode;
    g_cfg.ram_w = (uint16_t)w;
    g_cfg.ram_h = (uint16_t)h;
    g_cfg.rot = (uint8_t)(rot & 3);
    g_cfg.pixel_swap = (uint8_t)(pswap ? 1 : 0);
    g_cfg.addr_bytes = (uint8_t)(ab == 2 ? 2 : 4);
    g_cfg.madctl_style = (uint8_t)(style ? 1 : 0);
    if (smode >= 0 && smode <= 3) g_cfg.spi_mode = (uint8_t)smode;
    if (g_cfg.spi_mode != 1 && g_cfg.spi_mode != 3) g_cfg.spi_dma = 0;
    settings_save();
    bool test_running = test_gen_running();
    const char *msg = mode_changed ? "saved, reboot to apply spi mode" : "saved";
    if (mode_changed && test_running) test_gen_stop();
    if (size_changed) {
        bool was_test = test_gen_running();
        if (was_test) test_gen_stop();
        panel_apply_settings();
        proto_reset();
        if (was_test) {
            test_gen_start(test_gen_pattern(), test_gen_fps(), test_gen_mhz());
        }
    }
    panel_mark_all();
    char body_out[128];
    snprintf(body_out, sizeof(body_out), "{\"ok\":true,\"msg\":\"%s\"}", msg);
    resp_json(req, body_out);
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    char body[1024];
    if (json_body(req, body, sizeof(body)) < 0) {
        resp_json(req, "{\"ok\":false}");
        return ESP_OK;
    }
    char ssid[64];
    char pass[64];
    if (!jfind(body, "ssid", ssid, sizeof(ssid)) || !jfind(body, "pass", pass, sizeof(pass)) ||
        ssid[0] == 0 || pass[0] == 0) {
        resp_json(req, "{\"ok\":false,\"msg\":\"need ssid and pass\"}");
        return ESP_OK;
    }
    memset(g_cfg.sta_ssid, 0, sizeof(g_cfg.sta_ssid));
    memset(g_cfg.sta_pass, 0, sizeof(g_cfg.sta_pass));
    copy_str(g_cfg.sta_ssid, sizeof(g_cfg.sta_ssid), ssid);
    copy_str(g_cfg.sta_pass, sizeof(g_cfg.sta_pass), pass);
    nvs_force_ap_set(false);
    esp_err_t e = settings_save();
    resp_json(req, e == ESP_OK ? "{\"ok\":true,\"msg\":\"saved, rebooting into station mode\"}"
                               : "{\"ok\":false,\"msg\":\"save failed\"}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ap_handler(httpd_req_t *req)
{
    char body[1024];
    if (json_body(req, body, sizeof(body)) < 0) {
        resp_json(req, "{\"ok\":false}");
        return ESP_OK;
    }
    char ssid[64];
    char pass[64];
    if (!jfind(body, "ssid", ssid, sizeof(ssid)) || !jfind(body, "pass", pass, sizeof(pass))) {
        ssid[0] = 0;
        pass[0] = 0;
    }
    if (pass[0] == 0 || strlen(pass) < 8) {
        resp_json(req, "{\"ok\":false,\"msg\":\"AP password must be at least 8 chars\"}");
        return ESP_OK;
    }
    copy_str(g_cfg.ap_ssid, sizeof(g_cfg.ap_ssid), ssid);
    copy_str(g_cfg.ap_pass, sizeof(g_cfg.ap_pass), pass);
    esp_err_t e = settings_save();
    resp_json(req, e == ESP_OK ? "{\"ok\":true,\"msg\":\"saved, rebooting\"}"
                               : "{\"ok\":false,\"msg\":\"save failed\"}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t test_handler(httpd_req_t *req)
{
    char body[512];
    if (json_body(req, body, sizeof(body)) < 0) {
        resp_json(req, "{\"ok\":false}");
        return ESP_OK;
    }
    int enable = jint(body, "enable", test_gen_running() ? 1 : 0);
    int pattern = jint(body, "pattern", test_gen_pattern());
    int fps = jint(body, "fps", test_gen_fps());
    int mhz = jint(body, "mhz", test_gen_mhz());
    int lb = jint(body, "loopback", g_cfg.pin_loopback);

    bool lb_changed = ((lb ? 1 : 0) != (int)g_cfg.pin_loopback);
    g_cfg.pin_loopback = lb ? 1 : 0;
    if (lb_changed) settings_save();

    if (!enable) {
        test_gen_stop();
        resp_json(req, "{\"ok\":true,\"msg\":\"test stopped\"}");
        return ESP_OK;
    }

    esp_err_t e = test_gen_start(pattern, fps, mhz);
    if (e != ESP_OK) {
        char msg[96];
        snprintf(msg, sizeof(msg), "{\"ok\":false,\"msg\":\"start failed: %s\"}", esp_err_to_name(e));
        resp_json(req, msg);
        return ESP_OK;
    }
    resp_json(req, "{\"ok\":true,\"msg\":\"test running\"}");
    return ESP_OK;
}

static esp_err_t action_handler(httpd_req_t *req)
{
    char body[256];
    bool force_ap = false;
    bool reboot = false;
    const char *op = NULL;
    char opbuf[32];
    if (json_body(req, body, sizeof(body)) >= 0 && jfind(body, "op", opbuf, sizeof(opbuf))) {
        op = opbuf;
    }
    if (op) {
        if (!strcmp(op, "reboot")) reboot = true;
        else if (!strcmp(op, "ap_mode")) { force_ap = true; reboot = true; }
        else if (!strcmp(op, "factory")) { settings_reset(); reboot = true; }
    }
    resp_json(req, reboot ? "{\"ok\":true,\"msg\":\"rebooting\"}" : "{\"ok\":false,\"msg\":\"unknown op\"}");
    if (force_ap) nvs_force_ap_set(true);
    if (reboot) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}

#define INJ_BUF 8192
static uint8_t s_inj[INJ_BUF];

static esp_err_t inject_handler(httpd_req_t *req)
{
    int dc = 1;
    char q[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (strstr(q, "dc=0")) dc = 0;
    }
    size_t total = 0;
    while (total < sizeof(s_inj)) {
        int r = httpd_req_recv(req, (char *)(s_inj + total), sizeof(s_inj) - total);
        if (r < 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) break;
            resp_json(req, "{\"ok\":false,\"msg\":\"recv failed\"}");
            return ESP_OK;
        }
        if (r == 0) break;
        total += (size_t)r;
    }
    if (total == 0 || !panel_valid()) {
        resp_json(req, "{\"ok\":false,\"msg\":\"empty or no panel\"}");
        return ESP_OK;
    }
    proto_parse(dc != 0, s_inj, total);
    g_stat.inj_frames++;
    g_stat.inj_bytes += (uint32_t)total;
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"bytes\":%u,\"dc\":%d}", (unsigned)total, dc);
    resp_json(req, body);
    return ESP_OK;
}

static esp_err_t fb_handler(httpd_req_t *req)
{
    if (!panel_valid()) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no framebuffer");
        return ESP_OK;
    }
    size_t total = (size_t)panel_w() * panel_h() * 2;
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (size_t off = 0; off < total; off += sizeof(s_stage)) {
        size_t take = (total - off > sizeof(s_stage)) ? sizeof(s_stage) : (total - off);
        panel_copy_span(off, s_stage, take);
        esp_err_t e = httpd_resp_send_chunk(req, (const char *)s_stage, take);
        if (e != ESP_OK) return e;
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

void web_ui_start(void)
{
    if (s_started) return;
    s_started = true;
    s_cli_mux = xSemaphoreCreateMutex();
    if (!s_cli_mux) return;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_clients[i] = -1;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.lru_purge_enable = true;
    esp_err_t e = httpd_start(&s_http, &config);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed (%s)", esp_err_to_name(e));
        s_started = false;
        return;
    }
    ESP_LOGI(TAG, "httpd started on port %d", config.server_port);

    httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/index.html", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/display", .method = HTTP_POST, .handler = display_handler },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler },
        { .uri = "/api/ap", .method = HTTP_POST, .handler = ap_handler },
        { .uri = "/api/action", .method = HTTP_POST, .handler = action_handler },
        { .uri = "/api/test", .method = HTTP_POST, .handler = test_handler },
        { .uri = "/api/inject", .method = HTTP_POST, .handler = inject_handler },
        { .uri = "/api/fb", .method = HTTP_GET, .handler = fb_handler },
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
    };
    for (size_t k = 0; k < sizeof(uris) / sizeof(uris[0]); k++) {
        e = httpd_register_uri_handler(s_http, &uris[k]);
        if (e != ESP_OK) ESP_LOGE(TAG, "register %s failed (%s)", uris[k].uri, esp_err_to_name(e));
    }

    if (xTaskCreatePinnedToCore(stream_task, "ws_stream", 6144, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "stream task start failed");
    }
}
