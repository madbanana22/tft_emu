#include "panel_fb.h"
#include "tft_emu.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

static const char *TAG = "panel";

static uint16_t *s_fb;
static volatile uint16_t s_w, s_h;
static volatile uint8_t s_madctl;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_dirty[TFT_MAX_RAM_COLS / PANEL_TILE * TFT_MAX_RAM_ROWS / PANEL_TILE / 8 + 1];

static inline uint16_t tiles_w(void) { return (uint16_t)((s_w + PANEL_TILE - 1) / PANEL_TILE); }
static inline uint16_t tiles_h(void) { return (uint16_t)((s_h + PANEL_TILE - 1) / PANEL_TILE); }

static inline void dirty_idx(uint16_t t, bool set_reset)
{
    uint8_t m = (uint8_t)(1u << (t & 7));
    uint16_t b = t >> 3;
    if (b >= sizeof(s_dirty)) return;
    if (set_reset) s_dirty[b] |= m;
    else s_dirty[b] &= (uint8_t)~m;
}

esp_err_t panel_init(void)
{
    s_fb = NULL;
    s_w = 0;
    s_h = 0;
    s_madctl = 0;
    memset(s_dirty, 0xff, sizeof(s_dirty));
    return panel_apply_settings();
}

esp_err_t panel_apply_settings(void)
{
    uint16_t w = g_cfg.ram_w ? g_cfg.ram_w : 80;
    uint16_t h = g_cfg.ram_h ? g_cfg.ram_h : 160;
    size_t bytes = (size_t)w * h * 2;
    uint16_t *nfb = heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!nfb) {
        ESP_LOGE(TAG, "fb alloc %ux%u (%u bytes) failed", w, h, (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_mux);
    uint16_t *old = s_fb;
    s_fb = nfb;
    s_w = w;
    s_h = h;
    memset(s_dirty, 0xff, sizeof(s_dirty));
    portEXIT_CRITICAL(&s_mux);
    if (old) free(old);
    ESP_LOGI(TAG, "fb %ux%u @ %p (%u bytes)", w, h, nfb, (unsigned)bytes);
    return ESP_OK;
}

bool panel_valid(void) { return s_fb != NULL; }
uint16_t panel_w(void) { return s_w; }
uint16_t panel_h(void) { return s_h; }

void panel_set_madctl(uint8_t m)
{
    portENTER_CRITICAL(&s_mux);
    s_madctl = (uint8_t)(m & 0xE8);
    portEXIT_CRITICAL(&s_mux);
}

uint8_t panel_madctl(void) { return s_madctl; }

void panel_pixel(int16_t x, int16_t y, uint16_t c)
{
    int16_t gx = (int16_t)(x - g_cfg.off_x);
    int16_t gy = (int16_t)(y - g_cfg.off_y);
    if (gx < 0 || gy < 0 || gx >= (int16_t)s_w || gy >= (int16_t)s_h) return;
    s_fb[(uint32_t)gy * s_w + gx] = c;
    uint16_t t = (uint16_t)((gy / PANEL_TILE) * tiles_w() + gx / PANEL_TILE);
    dirty_idx(t, true);
}

void panel_fill(uint16_t c)
{
    portENTER_CRITICAL(&s_mux);
    if (s_fb) {
        uint32_t n = (uint32_t)s_w * s_h;
        for (uint32_t i = 0; i < n; i++) s_fb[i] = c;
    }
    memset(s_dirty, 0xff, sizeof(s_dirty));
    portEXIT_CRITICAL(&s_mux);
}

void panel_mark_all(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_dirty, 0xff, sizeof(s_dirty));
    portEXIT_CRITICAL(&s_mux);
}

bool panel_dirty_any(void)
{
    for (uint32_t i = 0; i < sizeof(s_dirty); i++) if (s_dirty[i]) return true;
    return false;
}

uint16_t panel_dirty_count(void)
{
    uint16_t n = 0;
    for (uint32_t i = 0; i < sizeof(s_dirty); i++) {
        uint8_t v = s_dirty[i];
        while (v) {
            v &= (uint8_t)(v - 1);
            n++;
        }
    }
    return n;
}

uint16_t panel_dirty_tiles(uint16_t *out, uint16_t max)
{
    uint16_t n = 0;
    if (!out) return 0;
    uint16_t tw = tiles_w();
    uint16_t total = (uint16_t)(tw * tiles_h());
    portENTER_CRITICAL(&s_mux);
    for (uint16_t t = 0; t < total; t++) {
        uint16_t b = (uint16_t)(t >> 3);
        if (b < sizeof(s_dirty) && (s_dirty[b] & (uint8_t)(1u << (t & 7))) && n < max) {
            out[n++] = t;
        }
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

void panel_clear_dirty(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_dirty, 0, sizeof(s_dirty));
    portEXIT_CRITICAL(&s_mux);
}

void panel_copy_span(size_t off, uint8_t *dst, size_t len)
{
    if (!s_fb || !dst || !len) return;
    size_t total = (size_t)s_w * s_h * 2;
    portENTER_CRITICAL(&s_mux);
    if (off < total) {
        size_t av = total - off;
        memcpy(dst, (uint8_t *)s_fb + off, len > av ? av : len);
    }
    portEXIT_CRITICAL(&s_mux);
}

void panel_copy_tile(uint16_t t, uint8_t *dst512)
{
    if (!s_fb) return;
    uint16_t tw = tiles_w();
    uint16_t tx = (uint16_t)((t % tw) * PANEL_TILE);
    uint16_t ty = (uint16_t)((t / tw) * PANEL_TILE);
    portENTER_CRITICAL(&s_mux);
    for (int iy = 0; iy < PANEL_TILE; iy++) {
        int16_t py = (int16_t)(ty + iy);
        for (int ix = 0; ix < PANEL_TILE; ix++) {
            int16_t px = (int16_t)(tx + ix);
            uint16_t c = 0;
            if (px < (int16_t)s_w && py < (int16_t)s_h) c = s_fb[(uint32_t)py * s_w + px];
            dst512[iy * PANEL_TILE * 2 + ix * 2] = (uint8_t)(c >> 8);
            dst512[iy * PANEL_TILE * 2 + ix * 2 + 1] = (uint8_t)c;
        }
    }
    dirty_idx(t, false);
    portEXIT_CRITICAL(&s_mux);
}
