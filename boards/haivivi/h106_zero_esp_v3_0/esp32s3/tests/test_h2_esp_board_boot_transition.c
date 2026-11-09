#include "h2_esp_board_boot_transition.h"

#include <assert.h>

static void test_error_page_power_off_marker(void) {
  /* Random RTC_NOINIT content after a real power loss is not a marker. */
  uint32_t marker = 0x12345678u;
  assert(!h2_esp_board_physical_off_marker_take(&marker));
  assert(marker == 0u);

  /* Error-page supervisor: no persisted transition, then a deep-sleep reset
   * under external power reports SHUTDOWN once. */
  h2_esp_board_physical_off_marker_set(&marker, true);
  bool physical_off = h2_esp_board_physical_off_marker_take(&marker);
  assert(physical_off);
  assert(h2_esp_board_boot_transition(
             false, 0u, H2_PAL_POWER_RESET_REASON_DEEP_SLEEP, physical_off) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN);
  assert(
      h2_esp_board_wake_transition(H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN) ==
      H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN);

  /* Taken on that boot, so a later deep-sleep standby is a sleep resume. */
  physical_off = h2_esp_board_physical_off_marker_take(&marker);
  assert(!physical_off);
  assert(h2_esp_board_boot_transition(
             false, 0u, H2_PAL_POWER_RESET_REASON_DEEP_SLEEP, physical_off) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP);

  /* A deep-sleep commit clears any marker. */
  h2_esp_board_physical_off_marker_set(&marker, true);
  h2_esp_board_physical_off_marker_set(&marker, false);
  assert(!h2_esp_board_physical_off_marker_take(&marker));

  /* The marker only applies to the deep-sleep reset that follows it. */
  assert(h2_esp_board_boot_transition(
             false, 0u, H2_PAL_POWER_RESET_REASON_POWER_ON, true) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_NONE);
}

static void test_persisted_transition(void) {
  /* Settings power-off persisted SHUTDOWN; the EXT1 wake keeps it. */
  const h2_pal_power_previous_transition_t shutdown =
      h2_esp_board_boot_transition(
          true, (uint32_t)H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN,
          H2_PAL_POWER_RESET_REASON_DEEP_SLEEP, false);
  assert(shutdown == H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN);
  assert(h2_esp_board_wake_transition(shutdown) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_SHUTDOWN);

  /* Deep-sleep standby persisted DEEP_SLEEP. */
  assert(h2_esp_board_boot_transition(
             true, (uint32_t)H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP,
             H2_PAL_POWER_RESET_REASON_DEEP_SLEEP,
             false) == H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP);
  assert(h2_esp_board_wake_transition(H2_PAL_POWER_PREVIOUS_TRANSITION_NONE) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_DEEP_SLEEP);

  assert(h2_esp_board_boot_transition(
             false, 0u, H2_PAL_POWER_RESET_REASON_SOFTWARE, false) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_REBOOT);
  assert(h2_esp_board_boot_transition(
             false, 0u, H2_PAL_POWER_RESET_REASON_POWER_ON, false) ==
         H2_PAL_POWER_PREVIOUS_TRANSITION_NONE);
}

int main(void) {
  test_error_page_power_off_marker();
  test_persisted_transition();
  return 0;
}
