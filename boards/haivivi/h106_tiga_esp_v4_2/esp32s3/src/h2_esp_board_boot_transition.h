#ifndef H2_ESP_BOARD_BOOT_TRANSITION_H
#define H2_ESP_BOARD_BOOT_TRANSITION_H

#include "hal/h2_pal_power.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Records whether the terminal commit is a physical power-off. */
void h2_esp_board_physical_off_marker_set(uint32_t* marker, bool physical_off);

/* Returns whether the previous commit was a physical power-off and clears the
 * marker, so it can affect only the boot that directly follows. */
bool h2_esp_board_physical_off_marker_take(uint32_t* marker);

/* Transition recorded for this boot: the persisted value when present,
 * otherwise derived from the reset reason. A physical-off marker turns the
 * deep-sleep reset that follows it into SHUTDOWN. */
h2_pal_power_previous_transition_t h2_esp_board_boot_transition(
    bool stored_found, uint32_t stored,
    h2_pal_power_reset_reason_t reset_reason, bool physical_off);

/* An EXT1 wake means the ESP was in deep sleep, but a physical power-off
 * with external power also ends there and keeps SHUTDOWN. */
h2_pal_power_previous_transition_t h2_esp_board_wake_transition(
    h2_pal_power_previous_transition_t transition);

#ifdef __cplusplus
}
#endif

#endif
