#include "h2_jieli_ac791n_devkit.h"

#include "usb/usb_common_def.h"

extern int usb_device_mode(const uint8_t usb_id, const uint32_t class_mask);
extern int h2_jieli_wl82_exception_log_clear(void) __attribute__((weak));

static int usb_debug_started;

const char *h2_jieli_ac791n_devkit_board_name(void) {
  return "jieli_ac791n_devkit";
}

h2_pal_result_t h2_jieli_ac791n_devkit_usb_debug_start(void) {
  if (usb_debug_started) return H2_PAL_OK;
  if (usb_device_mode(0, CDC_CLASS) != 0) return H2_PAL_ERR_IO;
  usb_debug_started = 1;
  return H2_PAL_OK;
}

/* Called by the patched SDK USB drain only after every byte in the captured
 * printf batch was accepted by CDC. A persisted exception remains intact
 * across disconnects, zero-byte writes and resets until this point. */
void h2_jieli_usb_debug_delivery_confirmed(void) {
  if (h2_jieli_wl82_exception_log_clear != NULL) {
    (void)h2_jieli_wl82_exception_log_clear();
  }
}
