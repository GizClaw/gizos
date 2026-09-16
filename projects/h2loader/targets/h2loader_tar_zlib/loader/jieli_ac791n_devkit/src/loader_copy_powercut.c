#include "asm/sfc_norflash_api.h"
#include "asm/wdt.h"
#include "os/os_api.h"
#include "h2_jieli_warm_request.h"

#include <stdint.h>
#include <stdio.h>

extern uint32_t boot_info_get_sfc_base_addr(void);

static int pause_on_erase;

static int header_erased(uint32_t address) {
  uint8_t bytes[32];
  if (norflash_origin_read(bytes, address, sizeof(bytes)) != (int)sizeof(bytes))
    return -1;
  for (unsigned i = 0; i < sizeof(bytes); ++i)
    if (bytes[i] != 0xffu) return 0;
  return 1;
}

/* Arm once per intact P1. After power loss the erased P1 header makes the
 * recovery boot skip this pause and retry the normal shared copy procedure.
 * This component is never part of the production Loader graph. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  if (stage != 105u ||
      boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE) return;
  pause_on_erase = header_erased(H2_JIELI_BANK_1_SFC_BASE - 32u) == 0;
}

void h2_jieli_upgrade_erase_observer(uint32_t address) {
  if (!pause_on_erase || address != H2_JIELI_BANK_1_SFC_BASE - 32u ||
      boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE) return;
  /* Do not request a power cut unless physical readback proves P1's header
   * erased and P2's recovery header published. Ordinary code still controls
   * publication and erasure ordering; the fixture only pauses afterward. */
  if (header_erased(address) != 1 ||
      header_erased(H2_JIELI_BANK_2_SFC_BASE - 32u) != 0) return;
  pause_on_erase = 0;
  wdt_close(); /* Hold this diagnostic boundary until physical power removal. */
  for (;;) {
    printf("H2_JIELI_POWERCUT_READY p1_header=erased p2_header=present\r\n");
    os_time_dly(100u);
  }
}
