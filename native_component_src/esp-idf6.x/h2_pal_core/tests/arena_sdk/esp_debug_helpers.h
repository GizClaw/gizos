#ifndef TEST_ARENA_ESP_DEBUG_HELPERS_H
#define TEST_ARENA_ESP_DEBUG_HELPERS_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t pc;
    uint32_t sp;
    uint32_t next_pc;
    const void *exc_frame;
} esp_backtrace_frame_t;
/* Matches ESP-IDF 76f5dedd9950a3012fee8fb7d5586df21fc67802:
 * components/esp_system/include/esp_debug_helpers.h, lines 62 and 82. */
void esp_backtrace_get_start(uint32_t *pc, uint32_t *sp, uint32_t *next_pc);
bool esp_backtrace_get_next_frame(esp_backtrace_frame_t *frame);
#endif
