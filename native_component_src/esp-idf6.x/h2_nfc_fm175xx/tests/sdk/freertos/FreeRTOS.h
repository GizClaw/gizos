#ifndef H2_NFC_TEST_FREERTOS_H
#define H2_NFC_TEST_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100u
#define pdMS_TO_TICKS(ms) ((TickType_t)((ms) * configTICK_RATE_HZ / 1000u))

#endif
