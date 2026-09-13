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
static int armed;
static int paused;

int h2_jieli_loader_powercut_paused(void) {
  return __atomic_load_n(&paused, __ATOMIC_ACQUIRE);
}

static int valid_header(uint32_t address) {
  uint8_t bytes[32];
  if (norflash_origin_read(bytes, address, sizeof(bytes)) != 32) return 0;
  (void)decode_data_by_user_key(0xffffu, bytes, 32u, 0u, 32u);
  return h2_jieli_native_u16(bytes) == h2_jieli_native_header_crc(bytes);
}

/* Only the first copy from an intact P1 is interrupted. A cold recovery
 * boot sees its invalid P1 header and retries without pausing again. */
void h2_jieli_wl82_boot_probe(uint32_t stage) {
  if (stage == 105u &&
      boot_info_get_sfc_base_addr() == H2_JIELI_BANK_2_SFC_BASE)
    armed = valid_header(H2_JIELI_BANK_1_SFC_BASE - 32u);
}

void h2_jieli_upgrade_write_observer(
    const uint8_t *data, uint32_t address, uint32_t size) {
  uint8_t bytes[32], expected[32], decoded[32];
  if (!armed || data == NULL || size < 32u ||
      address != H2_JIELI_BANK_1_SFC_BASE - 32u ||
      boot_info_get_sfc_base_addr() != H2_JIELI_BANK_2_SFC_BASE ||
      !valid_header(H2_JIELI_BANK_2_SFC_BASE - 32u)) return;
  if (norflash_origin_read(bytes, address, 32u) != 32) return;
  for (unsigned i = 0; i < 32u; ++i) if (bytes[i] != 0xffu) return;
  memset(expected, 0xff, sizeof(expected));
  memcpy(expected, data, 16u);
  memcpy(decoded, expected, sizeof(decoded));
  (void)decode_data_by_user_key(0xffffu, decoded, 32u, 0u, 32u);
  if (h2_jieli_native_u16(decoded) == h2_jieli_native_header_crc(decoded)) return;
  armed = 0;
  (void)norflash_protect_suspend();
  int written = norflash_write(NULL, (void *)data, 16u, address);
  (void)norflash_protect_resume();
  int read = norflash_origin_read(bytes, address, 32u);
  /* Prevent the caller's normal burn timeout from killing this diagnostic
   * task and clearing the physical header before the user cuts power. */
  __atomic_store_n(&paused, 1, __ATOMIC_RELEASE);
  wdt_close();
  for (;;) {
    printf("H2_JIELI_PARTIAL_P1_%s prefix=16 p2_crc=valid\r\n",
           written == 16 && read == 32 && !memcmp(bytes, expected, 32u)
               ? "READY" : "ERROR");
    os_time_dly(100u);
  }
}
