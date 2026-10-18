#ifndef PANEL_FB_H
#define PANEL_FB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

#define PANEL_TILE 16

esp_err_t panel_init(void);
esp_err_t panel_apply_settings(void);
bool panel_valid(void);
uint16_t panel_w(void);
uint16_t panel_h(void);

void panel_pixel(int16_t x, int16_t y, uint16_t c);
void panel_fill(uint16_t c);
void panel_mark_all(void);

void panel_set_madctl(uint8_t m);
uint8_t panel_madctl(void);

bool panel_dirty_any(void);
uint16_t panel_dirty_count(void);
uint16_t panel_dirty_tiles(uint16_t *out, uint16_t max);
void panel_clear_dirty(void);
void panel_copy_span(size_t off, uint8_t *dst, size_t len);
void panel_copy_tile(uint16_t t, uint8_t *dst512);

#endif
