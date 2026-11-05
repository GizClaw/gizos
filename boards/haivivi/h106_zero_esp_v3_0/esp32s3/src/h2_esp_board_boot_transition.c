#include "h2_esp_board_boot_transition.h"

#define H2_ESP_BOARD_PHYSICAL_OFF_MARKER 0x4f464632u

void h2_esp_board_physical_off_marker_set(uint32_t* marker, bool physical_off) {
  *marker = physical_off ? H2_ESP_BOARD_PHYSICAL_OFF_MARKER : 0u;
}

bool h2_esp_board_physical_off_marker_take(uint32_t* marker) {
  const bool physical_off = *marker == H2_ESP_BOARD_PHYSICAL_OFF_MARKER;
  *marker = 0u;
  return physical_off;
}

h2_pal_power_previous_transition_t h2_esp_board_boot_transition(
    bool stored_found, uint32_t stored,
    h2_pal_power_reset_reason_t reset_reason, bool physical_off) {
  if (reset_reason == H2_PAL_POWER_RESET_REASON_DEEP_SLEEP && physical_off)
    return H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN;
  if (stored_found) return (h2_pal_power_previous_transition_t)stored;
  if (reset_reason == H2_PAL_POWER_RESET_REASON_DEEP_SLEEP)
    return H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP;
  if (reset_reason == H2_PAL_POWER_RESET_REASON_SOFTWARE)
    return H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT;
  return H2_PAL_POWER_PREVIOUS_TRANSITION_NONE;
}

h2_pal_power_previous_transition_t h2_esp_board_wake_transition(
    h2_pal_power_previous_transition_t transition) {
  return transition == H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN
             ? H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN
             : H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP;
}
