#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    WIFI_MODE_NONE_EMU = 0,
    WIFI_MODE_STA_EMU,
    WIFI_MODE_AP_EMU,
} emu_wifi_mode_t;

void wifi_boot(void);
emu_wifi_mode_t wifi_get_mode(void);
const char *wifi_get_ip(void);
const char *wifi_get_ssid(void);

void wifi_default_ap_name(char *dst, size_t cap);

#endif
