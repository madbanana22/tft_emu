#ifndef TFT_EMU_H
#define TFT_EMU_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TFT_EMU_HVER 1

typedef struct {
    uint8_t  hver;          /* settings format version */
    uint16_t ram_w;         /* emulated panel RAM width  (columns) */
    uint16_t ram_h;         /* emulated panel RAM height (rows) */
    uint8_t  off_x;         /* RAM -> glass offset (ST7735 red-tab style offsets) */
    uint8_t  off_y;
    uint8_t  rot;           /* extra browser-side rotation: 0/1/2/3 (90deg steps) */
    uint8_t  pixel_swap;    /* invert 16-bit pixel byte order from host */
    uint8_t  addr_bytes;    /* CASET/RASET parameter width: 4 or 2 */
    uint8_t  madctl_style;  /* 0: MX/MY map to X/Y, 1: swapped variant */
    uint8_t  spi_mode;      /* 0..3 */
    uint8_t  spi_dma;       /* 0/1; ESP32 HW only supports DMA in modes 1/3 */
    char     sta_ssid[33];
    char     sta_pass[65];
    char     ap_ssid[33];
    char     ap_pass[65];
    uint8_t  pin_loopback;   /* test master drives the emulator GPIO pads directly (no jumpers) */
} tft_settings_t;

extern tft_settings_t g_cfg;

enum {
    TFT_MAX_RAM_COLS = 255,   /* keeps 16-bit address parsing unambiguous on 8-bit CASET bytes */
    TFT_MAX_RAM_ROWS = 255,
    TFT_MAX_RAM_BYTES = 240 * 320 * 2, /* hard cap for internal-RAM framebuffer on ESP32 */
};

typedef struct {
    volatile uint32_t spi_trans_cnt;
    volatile uint32_t spi_rx_bytes;
    volatile uint32_t spi_full_burst;      /* transactions that filled the whole rx buffer (possible truncation) */
    volatile uint32_t proto_cmd_cnt;
    volatile uint32_t proto_err_cnt;
    volatile uint32_t mem_tx_frames;       /* frames sent over websocket */
    volatile uint32_t mem_tx_bytes;
    volatile uint32_t inj_frames;          /* code injections via /api/inject */
    volatile uint32_t inj_bytes;
    volatile uint32_t cs_edges;            /* CS falling edges seen (DC ring pushes) */
    volatile uint32_t dc0_cnt;             /* transactions tagged DC=0 (commands) */
    volatile uint32_t dc1_cnt;             /* transactions tagged DC=1 (pixel data) */
    volatile uint32_t big_bursts;          /* transactions > 1024 bytes */
    volatile uint32_t mis_bursts;          /* transactions with non-word-aligned length (DMA tail loss risk) */
    volatile uint32_t ramwr_cnt;           /* RAMWR commands parsed */
    volatile uint32_t pix_cnt;             /* pixel writes into the framebuffer */
} tft_stats_t;

extern tft_stats_t g_stat;

const char * tft_ver(void);

#endif
