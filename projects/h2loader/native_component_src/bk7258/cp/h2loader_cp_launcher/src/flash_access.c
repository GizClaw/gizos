#include "flash_access.h"

#ifdef H2_BK_CP_FLASH_ACCESS_HOST_TEST
#include "flash_access_test_sdk.h"
#else
#include "driver/flash.h"
#include "driver/efuse.h"
#include "driver/aon_rtc.h"
#include "driver/mb_ipc.h"
#include "efuse_ll.h"
#include "h2_bk_platform_core.h"
#include "os/os.h"
#include "armstar.h"
#endif
#include <string.h>

static unsigned s_aes = H2_BK_FLASH_AES_UNKNOWN;
static beken_semaphore_t s_events;
static uint32_t s_server;
static h2_pal_task_t* s_task;
static uint8_t s_replies[3][16];
static uint8_t s_replied[3];

static unsigned read_aes_control(void) {
#if !CONFIG_EFUSE || CONFIG_CACHE_ENABLE || CONFIG_FLASH_CP_AP_DIRECT_ACCESS
  return H2_BK_FLASH_AES_UNKNOWN;
#else
  if (bk_efuse_driver_init() != BK_OK) return H2_BK_FLASH_AES_UNKNOWN;
  efuse_hw_t* hw = (efuse_hw_t*)EFUSE_LL_REG_BASE(0);
  const uint32_t flags = rtos_disable_int();
  unsigned state = H2_BK_FLASH_AES_UNKNOWN;
  if (efuse_ll_is_operate_finished(hw)) {
    const uint64_t start = bk_aon_rtc_get_us();
    efuse_ll_set_direction_read(hw);
    efuse_ll_set_addr(hw, 3);
    efuse_ll_enable(hw);
    for (unsigned i = 0; i < 10000; ++i) {
      if (efuse_ll_is_operate_finished(hw)) {
        if (efuse_ll_is_rd_data_valid(hw))
          state = (efuse_ll_get_rd_data(hw) >> 5) & 1u;
        break;
      }
      if (bk_aon_rtc_get_us() - start >= 5000u) break;
    }
    efuse_ll_disable(hw);
  }
  rtos_enable_int(flags);
  return state;
#endif
}

#ifdef H2_BK_CP_FLASH_ACCESS_HOST_TEST
unsigned h2_bk_flash_test_read_control(void) { return read_aes_control(); }
#endif

int h2_bk_flash_cp_vector(uint32_t physical, uint32_t vector[2]) {
  uint8_t packet[34];
  if (!vector || physical % 34u || physical > 0x1000000u - 34u) return -1;
  if (bk_flash_read_bytes(physical, packet, sizeof(packet)) != BK_OK ||
      h2_bk_flash_erased(packet) || !h2_bk_flash_crc_valid(packet))
    return -1;
  const uintptr_t address = 0x02000000u + physical / 34u * 32u;
  const uint32_t flags = rtos_disable_int();
#ifdef H2_BK_CP_FLASH_ACCESS_HOST_TEST
  h2_bk_test_cp_vector_copy(address, vector);
#else
  SCB_InvalidateDCache_by_Addr((void*)address, 32);
  __DSB();
  memcpy(vector, (const void*)address, 8);
#endif
  rtos_enable_int(flags);
  return 0;
}

/* The SDK callback uses u32 (unsigned int), unlike this libc's uint32_t. */
static unsigned int status_event(unsigned int handle, unsigned int connection) {
  (void)handle;
  if (connection < 3u) (void)rtos_set_semaphore(&s_events);
  return 0;
}

static void status_worker(void* user) {
  (void)user;
  for (;;) {
    if (rtos_get_semaphore(&s_events, BEKEN_WAIT_FOREVER) != BK_OK) continue;
    for (unsigned i = 0; i < 3u; ++i) {
      if (s_replied[i]) continue;
      uint32_t peer = mb_ipc_server_get_connect_handle(s_server, i);
      unsigned int event = 0;
      if (mb_ipc_get_recv_event(peer, &event) != 0 || event != MB_IPC_SEND_CMD)
        continue;
      uint8_t request[16], command = 0;
      if (mb_ipc_get_recv_data_len(peer) != sizeof(request)) {
        (void)mb_ipc_recv(peer, &command, NULL, 0, 0);
        continue;
      }
      int count = mb_ipc_recv(peer, &command, request, sizeof(request), 0);
      if (count != sizeof(request) || command != H2_BK_FLASH_STATUS_COMMAND ||
          !h2_bk_flash_status_valid(request, 0))
        continue;
      memcpy(s_replies[i], request, sizeof(request));
      s_replies[i][12] = (uint8_t)s_aes;
      /* A response buffer is never recycled, even if send times out. The AP
       * queries once per boot; reconnect/repeated traffic is not a new read. */
      s_replied[i] = 1;
      (void)mb_ipc_send(peer, command, s_replies[i], sizeof(s_replies[i]), 500);
    }
  }
}

int h2_bk_flash_status_start(void) {
  if (s_server) return 0;
  s_aes = read_aes_control();
  if (rtos_init_semaphore(&s_events, 1) != BK_OK) return -1;
  s_server = mb_ipc_socket(H2_BK_FLASH_STATUS_SERVER, status_event);
  if (!s_server) {
    (void)rtos_deinit_semaphore(&s_events);
    return -1;
  }
  const h2_pal_task_options_t options = {.name = "h2loader/flash-status",
                                         .min_stack_size = 4096};
  if (h2_pal_task_start(h2_bk_platform_task_api(), &options, status_worker,
                        NULL, &s_task) != H2_PAL_OK) {
    (void)mb_ipc_server_close(s_server, 500);
    s_server = 0;
    (void)rtos_deinit_semaphore(&s_events);
    return -1;
  }
  return 0;
}
