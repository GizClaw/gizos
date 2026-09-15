#include "asm/sfc_norflash_api.h"
#include "os/os_api.h"
#include "h2_jieli_warm_request.h"
#include "jieli_native_image.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern uint32_t boot_info_get_sfc_base_addr(void);
extern int decode_data_by_user_key(uint16_t, uint8_t *, uint16_t,
                                   uint32_t, uint8_t);

#ifndef H2_JIELI_TEST_PREFIX_BYTES
#define H2_JIELI_TEST_PREFIX_BYTES 16u
#endif
_Static_assert(H2_JIELI_TEST_PREFIX_BYTES == 16u || H2_JIELI_TEST_PREFIX_BYTES == 32u,
               "Only the audited torn-header sizes are supported");
extern void system_reset(void);

static void reset_without_publication(void) {
  os_time_dly(100u);
  system_reset();
  for (;;) { }
}

/* Test-only: leave a deterministically invalid P2 header with P1 untouched.
 * A cold restart should select P1. Never link into a production package. */
void h2_jieli_upgrade_publish_observer(const uint8_t header[32]) {
  uint8_t partial[32], physical[32], p1[32];
  const uint32_t address = H2_JIELI_BANK_2_SFC_BASE - 32u;
  if (boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE) return;
  if (norflash_origin_read(p1, H2_JIELI_BANK_1_SFC_BASE - 32u, 32u) != 32)
    goto rejected;
  (void)decode_data_by_user_key(0xffffu, p1, 32u, 0u, 32u);
  if (h2_jieli_native_u16(p1) != h2_jieli_native_header_crc(p1)) goto rejected;
  memset(partial, 0xff, sizeof(partial));
  memcpy(partial, header, H2_JIELI_TEST_PREFIX_BYTES);
  /* Full-length programming interruption: retain an intentionally wrong byte. */
  if (H2_JIELI_TEST_PREFIX_BYTES == 32u) partial[31] ^= 1u;
  memcpy(physical, partial, sizeof(physical));
  (void)decode_data_by_user_key(0xffffu, physical, 32u, 0u, 32u);
  if (h2_jieli_native_u16(physical) == h2_jieli_native_header_crc(physical))
    goto rejected; /* Do not test a prefix that accidentally passes header CRC. */
  (void)norflash_protect_suspend();
  int written = norflash_write(NULL, partial, H2_JIELI_TEST_PREFIX_BYTES, address);
  (void)norflash_protect_resume();
  int read = norflash_origin_read(physical, address, 32u);
  const int ready = written == (int)H2_JIELI_TEST_PREFIX_BYTES && read == 32 &&
                    memcmp(partial, physical, 32u) == 0;
  printf("H2_JIELI_PARTIAL_HEADER_%s bank=2 prefix=%u p1_crc=valid reset=software\r\n",
         ready ? "READY" : "ERROR", (unsigned)H2_JIELI_TEST_PREFIX_BYTES);
  /* Bytes are already on NOR. Reset tests the same ROM header selection as a
   * power cut at this point; it does not simulate a cut during programming. */
  reset_without_publication();
  return;
rejected:
  printf("H2_JIELI_PARTIAL_HEADER_REJECTED bank=2 reset=software\r\n");
  reset_without_publication();
}
