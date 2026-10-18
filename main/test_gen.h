#ifndef TEST_GEN_H
#define TEST_GEN_H

#include <stdbool.h>
#include <esp_err.h>

typedef enum {
    TFT_TEST_RAINBOW = 0,
    TFT_TEST_PROGRESS,
    TFT_TEST_BARS,
    TFT_TEST_CHECKER,
    TFT_TEST_SOLID,
    TFT_TEST_GRADIENT,
    TFT_TEST_PATTERN_COUNT
} tft_test_pattern_t;

esp_err_t test_gen_start(int pattern, int fps, int mhz);
esp_err_t test_gen_stop(void);
bool test_gen_running(void);
int test_gen_pattern(void);
int test_gen_fps(void);
int test_gen_mhz(void);
const char *test_gen_pattern_name(int pattern);

#endif
