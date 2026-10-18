#ifndef NVS_STORE_H
#define NVS_STORE_H

#include <esp_err.h>
#include "tft_emu.h"

esp_err_t settings_load(void);
esp_err_t settings_save(void);
esp_err_t settings_reset(void);

bool nvs_force_ap_get(void);          /* returns and clears the one-shot AP-next-boot flag */
void nvs_force_ap_set(bool enabled);

#endif
