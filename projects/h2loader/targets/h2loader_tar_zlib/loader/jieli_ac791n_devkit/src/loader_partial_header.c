#include "asm/sfc_norflash_api.h"
#include "asm/wdt.h"
#include "os/os_api.h"
#include "h2_jieli_warm_request.h"
#include "jieli_native_image.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t boot_info_get_sfc_base_addr(void);
extern int decode_data_by_user_key(uint16_t, uint8_t *, uint16_t,
                                   uint32_t, uint8_t);

/* Test-only: leave a deterministically invalid P2 header with P1 untouched.
 * A cold restart should select P1. Never link into a production package. */
void h2_jieli_upgrade_publish_observer(const uint8_t header[32]) {
  uint8_t partial[32], physical[32], p1[32];
  const uint32_t address = H2_JIELI_BANK_2_SFC_BASE - 32u;
  if (boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE) return;
  if (norflash_origin_read(p1, H2_JIELI_BANK_1_SFC_BASE - 32u, 32u) != 32)
    return;
  (void)decode_data_by_user_key(0xffffu, p1, 32u, 0u, 32u);
  if (h2_jieli_native_u16(p1) != h2_jieli_native_header_crc(p1)) return;
  memset(partial, 0xff, sizeof(partial));
  memcpy(partial, header, 16u);
  memcpy(physical, partial, sizeof(physical));
  (void)decode_data_by_user_key(0xffffu, physical, 32u, 0u, 32u);
  if (h2_jieli_native_u16(physical) == h2_jieli_native_header_crc(physical))
    return; /* Do not test a prefix that accidentally passes header CRC. */
  (void)norflash_protect_suspend();
  int written = norflash_write(NULL, partial, 16u, address);
  (void)norflash_protect_resume();
  int read = norflash_origin_read(physical, address, 32u);
  wdt_close();
  for (;;) {
    printf("H2_JIELI_PARTIAL_HEADER_%s bank=2 prefix=16 p1_crc=valid\r\n",
           written == 16 && read == 32 &&
           memcmp(partial, physical, 32u) == 0 ? "READY" : "ERROR");
    os_time_dly(100u);
  }
}
