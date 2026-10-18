#include "proto_st7735.h"
#include "panel_fb.h"
#include "tft_emu.h"

#include <string.h>
#include <esp_log.h>

#define CMD_SWRESET 0x01
#define CMD_NOP     0x00
#define CMD_INVOFF  0x20
#define CMD_INVON   0x21
#define CMD_DISPOFF 0x28
#define CMD_DISPON  0x29
#define CMD_CASET   0x2A
#define CMD_RASET   0x2B
#define CMD_RAMWR   0x2C
#define CMD_MADCTL  0x36
#define CMD_COLMOD  0x3A

#define MAD_MV  0x20
#define MAD_MX  0x40
#define MAD_MY  0x80
#define MAD_BGR 0x08

typedef struct {
    int16_t win_x0, win_x1, win_y0, win_y1;
    uint32_t n;
    uint8_t parity;
    uint8_t p_hi;
    bool writing;
} proto_st_t;

static proto_st_t g_st;

void proto_reset(void)
{
    memset(&g_st, 0, sizeof(g_st));
    g_st.win_x0 = 0;
    g_st.win_y0 = 0;
    g_st.win_x1 = (int16_t)(panel_w() ? panel_w() - 1 : 0);
    g_st.win_y1 = (int16_t)(panel_h() ? panel_h() - 1 : 0);
    panel_set_madctl(0x00);
    panel_fill(0x0000);
}

static void pix_write_raw(uint16_t c)
{
    uint8_t mad = panel_madctl();
    if (mad & MAD_BGR) {
        uint16_t r = (uint16_t)((c >> 11) & 0x1f);
        uint16_t b = (uint16_t)(c & 0x1f);
        c = (uint16_t)((c & 0x07E0) | (b << 11) | r);
    }
    int32_t w = g_st.win_x1 - g_st.win_x0 + 1;
    int32_t h = g_st.win_y1 - g_st.win_y0 + 1;
    if (w < 1 || h < 1) return;
    int32_t ax, ay;
    if (mad & MAD_MV) {
        ay = g_st.win_y0 + (int32_t)(g_st.n % (uint32_t)h);
        ax = g_st.win_x0 + (int32_t)(g_st.n / (uint32_t)h);
    } else {
        ax = g_st.win_x0 + (int32_t)(g_st.n % (uint32_t)w);
        ay = g_st.win_y0 + (int32_t)(g_st.n / (uint32_t)w);
    }
    g_st.n++;
    if (g_st.n >= (uint32_t)(w * h)) g_st.n = 0;
    panel_pixel((int16_t)ax, (int16_t)ay, c);
    g_stat.pix_cnt++;
}

static void pix_bytes(const uint8_t *b, size_t len)
{
    bool swap = g_cfg.pixel_swap;
    uint8_t parity = g_st.parity;
    while (len--) {
        uint8_t v = *b++;
        if (parity == 0) {
            g_st.p_hi = v;
        } else {
            uint16_t c = swap ? ((uint16_t)v << 8) | g_st.p_hi : ((uint16_t)g_st.p_hi << 8) | v;
            pix_write_raw(c);
        }
        parity ^= 1;
    }
    g_st.parity = parity;
}

static bool cand_w(const uint8_t *p, size_t avail, int argw, int exp_next, uint16_t *v0, uint16_t *v1)
{
    if (argw == 4) {
        if (avail < 4) return false;
        uint16_t x0 = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t x1 = (uint16_t)((p[2] << 8) | p[3]);
        if (x0 > x1 || x1 >= 256) return false;
        if (avail > 4 && p[4] != (uint8_t)exp_next) return false;
        *v0 = x0;
        *v1 = x1;
        return true;
    }
    if (avail < 2) return false;
    if (p[0] > p[1]) return false;
    if (avail > 2 && p[2] != (uint8_t)exp_next) return false;
    *v0 = p[0];
    *v1 = p[1];
    return true;
}

void proto_parse(bool dc, const uint8_t *buf, size_t n)
{
    if (!panel_valid() || !n) return;
    if (dc) {
        if (g_st.writing) {
            pix_bytes(buf, n);
        }
        return;
    }

    const uint8_t *end = buf + n;
    const uint8_t *p = buf;
    while (p < end) {
        uint8_t op = *p++;
        g_st.writing = false;
        g_stat.proto_cmd_cnt++;
        switch (op) {
        case CMD_SWRESET:
            proto_reset();
            break;
        case CMD_CASET: {
            uint16_t a4_0, a4_1, a2_0, a2_1;
            bool ok4 = cand_w(p, end - p, 4, CMD_RASET, &a4_0, &a4_1);
            bool ok2 = cand_w(p, end - p, 2, CMD_RASET, &a2_0, &a2_1);
            int want = (g_cfg.addr_bytes == 2) ? 2 : 4;
            if (want == 4 && ok4) {
                g_st.win_x0 = (int16_t)a4_0;
                g_st.win_x1 = (int16_t)a4_1;
                p += 4;
            } else if (want == 2 && ok2) {
                g_st.win_x0 = (int16_t)a2_0;
                g_st.win_x1 = (int16_t)a2_1;
                p += 2;
            } else if (ok4) {
                g_st.win_x0 = (int16_t)a4_0;
                g_st.win_x1 = (int16_t)a4_1;
                p += 4;
            } else if (ok2) {
                g_st.win_x0 = (int16_t)a2_0;
                g_st.win_x1 = (int16_t)a2_1;
                p += 2;
            } else {
                g_stat.proto_err_cnt++;
                return;
            }
            g_st.n = 0;
            g_st.parity = 0;
            break;
        }
        case CMD_RASET: {
            uint16_t a4_0, a4_1, a2_0, a2_1;
            bool ok4 = cand_w(p, end - p, 4, CMD_RAMWR, &a4_0, &a4_1);
            bool ok2 = cand_w(p, end - p, 2, CMD_RAMWR, &a2_0, &a2_1);
            int want = (g_cfg.addr_bytes == 2) ? 2 : 4;
            if (want == 4 && ok4) {
                g_st.win_y0 = (int16_t)a4_0;
                g_st.win_y1 = (int16_t)a4_1;
                p += 4;
            } else if (want == 2 && ok2) {
                g_st.win_y0 = (int16_t)a2_0;
                g_st.win_y1 = (int16_t)a2_1;
                p += 2;
            } else if (ok4) {
                g_st.win_y0 = (int16_t)a4_0;
                g_st.win_y1 = (int16_t)a4_1;
                p += 4;
            } else if (ok2) {
                g_st.win_y0 = (int16_t)a2_0;
                g_st.win_y1 = (int16_t)a2_1;
                p += 2;
            } else {
                g_stat.proto_err_cnt++;
                return;
            }
            g_st.n = 0;
            g_st.parity = 0;
            break;
        }
        case CMD_MADCTL:
            if (end - p < 1) { g_stat.proto_err_cnt++; return; }
            panel_set_madctl(p[0]);
            p += 1;
            break;
        case CMD_COLMOD:
            if (end - p < 1) { g_stat.proto_err_cnt++; return; }
            p += 1;
            break;
        case CMD_RAMWR:
            g_stat.ramwr_cnt++;
            g_st.writing = true;
            p = end;
            break;
        case CMD_NOP:
        case CMD_INVON:
        case CMD_INVOFF:
        case CMD_DISPON:
        case CMD_DISPOFF:
            break;
        default:
            p = end;
            break;
        }
    }
}
