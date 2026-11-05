#include "h2_bk_flash_access.h"
#include "h2_bk_flash_access_test_sdk.h"
#include "flash_access.h"
#include <assert.h>
#include <string.h>

#define BASE 0x264000u
static uint8_t packets[18 * 34], plain[18 * 32];
static int critical, mapped, raw_reads, invalidated, barrier;
static uint8_t native_slot;
static int raw_read_error, partitions_missing;
#define BOOT_WINDOW 0x11000u
static const bk_logic_partition_t own_window = {BOOT_WINDOW};
static const bk_logic_partition_t other_window = {BASE};
const bk_logic_partition_t* bk_flash_partition_get_info(uint32_t id) {
  if (partitions_missing) return NULL;
  return id == BK_PARTITION_APPLICATION ? &own_window : &other_window;
}
static void make_packets(void) {
  for (unsigned i = 0; i < sizeof(plain); i++) plain[i] = (uint8_t)i;
  for (unsigned b = 0; b < 18; b++) {
    uint8_t* p = packets + b * 34;
    memcpy(p, plain + b * 32, 32);
    uint16_t c = 0xffff;
    for (unsigned i = 0; i < 32; i++) {
      c ^= (uint16_t)p[i] << 8;
      for (unsigned k = 0; k < 8; k++)
        c = (uint16_t)((c << 1) ^ ((c & 0x8000) ? 0x8005 : 0));
    }
    p[32] = (uint8_t)(c >> 8);
    p[33] = (uint8_t)c;
  }
}
int bk_flash_read_bytes(uint32_t p, uint8_t* out, uint32_t n) {
  /* The same packets model the boot window at physical zero and the App
   * window at BASE; anything else is a read outside the permitted region. */
  const uint32_t base = p < sizeof(packets) ? 0u : BASE;
  assert(!critical && p >= base && p + n <= base + sizeof(packets));
  if (raw_read_error) return -1;
  memcpy(out, packets + (p - base), n);
  raw_reads++;
  return 0;
}
uint8_t bk_ota_get_current_partition(void) { return native_slot; }
uint32_t rtos_enter_critical(void) {
  assert(!critical);
  critical = 1;
  return 7;
}
void rtos_exit_critical(uint32_t f) {
  assert(f == 7 && critical);
  critical = 0;
}
void SCB_InvalidateDCache_by_Addr(void* p, int32_t n) {
  assert(critical && n == 32);
  (void)p;
  invalidated = 1;
  barrier = 0;
}
void __DSB(void) {
  assert(invalidated && critical);
  barrier = 1;
}
void h2_bk_test_mapped_copy(uintptr_t a, uint32_t within, void* out, size_t n) {
  const uintptr_t base = a >= 0x2240000u ? 0x2240000u : 0x2000000u;
  assert(critical && invalidated && barrier && a >= base &&
         a + within + n <= base + sizeof(plain));
  memcpy(out, plain + (a - base + within), n);
  mapped++;
  invalidated = barrier = 0;
}
uint32_t mb_ipc_socket(uint8_t p, void* cb) {
  assert(p == 19 && !cb);
  return 1;
}
int mb_ipc_connect(uint32_t h, uint8_t cpu, uint8_t port, uint32_t t) {
  assert(h == 1 && cpu == 0 && port == 4 && t == 500);
  return 0;
}
int mb_ipc_send(uint32_t h, uint8_t cmd, uint8_t* d, uint32_t n, uint32_t t) {
  assert(h == 1 && cmd == 1 && n == 16 && t == 500 &&
         h2_bk_flash_status_valid(d, 0));
  return 0;
}
int mb_ipc_recv(uint32_t h, uint8_t* cmd, uint8_t* d, uint32_t n, uint32_t t) {
  assert(h == 1 && n == 16 && t == 500);
  *cmd = 1;
  h2_bk_flash_status_request(d);
  d[12] = 1;
#ifdef TEST_UNKNOWN
  return -1;
#else
  return 16;
#endif
}
int main(void) {
  uint8_t out[sizeof(plain)];
  make_packets();
  assert(h2_bk_flash_read_plain(BASE, 68, 7, out, 50) == 0 &&
         !memcmp(out, plain + 7, 50));
  assert(mapped == 2 && raw_reads == 1 && !critical);
  mapped = raw_reads = 0;
  assert(h2_bk_flash_read_plain(BASE, sizeof(packets), 0, out, sizeof(out)) ==
             0 &&
         !memcmp(out, plain, sizeof(out)));
  assert(mapped == 18 && raw_reads == 2);
  packets[0] ^= 1;
  mapped = 0;
  assert(h2_bk_flash_read_plain(BASE, 68, 0, out, 1) < 0 && !mapped);
  memset(packets, 255, 34);
  assert(h2_bk_flash_read_plain(BASE, 68, 0, out, 32) == 0);
  for (unsigned i = 0; i < 32; i++) assert(out[i] == 255);
  assert(!mapped);
  assert(h2_bk_flash_read_plain(BASE + 1, 68, 0, out, 1) < 0);
  assert(h2_bk_flash_read_plain(BASE, 68, 64, out, 1) < 0);
  native_slot = 1;
  assert(h2_bk_flash_read_plain(BASE, 68, 0, out, 1) == H2_PAL_ERR_UNSUPPORTED);

  /* Native B keeps the boot window readable, and only the boot window: the
   * generic reader still refuses it and every executable window. */
  make_packets();
  mapped = raw_reads = 0;
  memset(out, 0, sizeof(out));
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW, 7, out, 50) == H2_PAL_OK &&
         !memcmp(out, plain + 7, 50));
  assert(mapped == 2 && raw_reads == 1 && !critical);
  assert(h2_bk_flash_read_plain(0, sizeof(packets), 0, out, 1) ==
         H2_PAL_ERR_UNSUPPORTED);
  mapped = raw_reads = 0;
  assert(h2_bk_flash_read_boot_plain(0, 0, out, 1) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW + 34u, 0, out, 1) ==
         H2_PAL_ERR_INVALID_ARG);
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW, BOOT_WINDOW / 34u * 32u - 1u,
                                     out, 2) == H2_PAL_ERR_INVALID_ARG);
  partitions_missing = 1;
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW, 0, out, 1) ==
         H2_PAL_ERR_INVALID_ARG);
  partitions_missing = 0;
  assert(!raw_reads && !mapped);
  raw_read_error = 1;
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW, 0, out, 1) == H2_PAL_ERR_IO);
  raw_read_error = 0;
  packets[0] ^= 1;
  assert(h2_bk_flash_read_boot_plain(BOOT_WINDOW, 0, out, 1) == H2_PAL_ERR_IO);
  assert(!mapped && !critical);
#ifdef TEST_UNKNOWN
  assert(h2_bk_flash_aes_state() == H2_BK_FLASH_AES_UNKNOWN);
#else
  assert(h2_bk_flash_aes_state() == H2_BK_FLASH_AES_ON);
#endif
  return 0;
}
