#ifndef PROTO_ST7735_H
#define PROTO_ST7735_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

void proto_reset(void);
void proto_parse(bool dc, const uint8_t *buf, size_t n);

#endif
