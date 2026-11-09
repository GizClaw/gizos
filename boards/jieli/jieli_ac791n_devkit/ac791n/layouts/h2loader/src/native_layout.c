#include "h2_jieli_wl82_boot.h"
#include "h2_jieli_ac791n_devkit_partitions.h"
#include "h2_jieli_warm_request.h"

/* Available before setup_arch, task startup, and Runtime construction. */
const h2_jieli_wl82_boot_geometry_t* h2_jieli_wl82_boot_geometry(void) {
  static const h2_jieli_wl82_boot_geometry_t geometry = {
      .bank_1_sfc_base = H2_JIELI_BANK_1_SFC_BASE,
      .bank_2_sfc_base = H2_JIELI_BANK_2_SFC_BASE,
      .coredump_flash_address = H2_JIELI_COREDUMP_ADDRESS,
  };
  return &geometry;
}

#include "app_config.h"
#include "h2_jieli_ac791n_devkit.h"
#include "h2_jieli_wl82_console.h"

const h2_jieli_wl82_console_config_t* h2_jieli_wl82_console_board_config(void) {
  static const h2_jieli_wl82_console_config_t config = {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
      .start = h2_jieli_ac791n_devkit_console_start,
      .read = h2_jieli_ac791n_devkit_console_read,
      .write = h2_jieli_ac791n_devkit_console_write,
#else
      .start = h2_jieli_ac791n_devkit_usb_debug_start,
#endif
  };
  return &config;
}
