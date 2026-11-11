#ifdef NDEBUG
#undef NDEBUG
#endif
#include "sdk.h"
#include <assert.h>
#include <stddef.h>
#include <string.h>
#define H2_BK_UART_IO_TEST 1
#include "native_component_src/bk7258/ap/h2_pal_core/src/h2_bk_platform_uart_io_stream.c"
static char wire[4096];
static size_t wire_size;
static int competing_attempts;
static uint32_t now_ms, lock_until, drain_until;
static int capacity, accepted, suspended, resumes, byte_error;
static uint32_t write_drain_ms;
static int awake, votes, pm_error, baud_error, resume_ms;
static int sdk_pending, sdk_outputs;
static int control(shell_dev_t* dev, int cmd, void* arg) {
  (void)dev;
  (void)arg;
  if (cmd == SHELL_IO_CTRL_TX_SUSPEND) {
    assert(now_ms >= drain_until);
    suspended = 1;
  } else {
    assert(suspended);
    if (sdk_pending) {
      /* The queued SDK producer may resume only after direct FIFO data. */
      assert(now_ms >= drain_until);
      memcpy(wire + wire_size, "SDK", 3);
      wire_size += 3;
      sdk_pending = 0;
      sdk_outputs++;
    }
    suspended = 0;
    resumes++;
    resume_ms = (int)now_ms;
  }
  return 1;
}
static shell_drv_t driver = {control};
shell_dev_t shell_uart = {&driver};
uint32_t rtos_get_time(void) { return now_ms; }
uint32_t rtos_enter_critical(void) {
  if (lock_until != 0u) {
    s_direct_tx_busy = now_ms < lock_until;
    if (now_ms >= lock_until) lock_until = 0u;
  }
  return 0;
}
void rtos_exit_critical(uint32_t l) { (void)l; }
void rtos_delay_milliseconds(uint32_t n) { now_ms += n; }
int bk_uart_set_baud_rate(uart_id_t id, uint32_t n) {
  (void)id;
  (void)n;
  return baud_error;
}
int bk_pm_module_vote_sleep_ctrl(pm_sleep_module_name_e module, uint32_t state,
                                 uint32_t time) {
  assert(module == PM_SLEEP_MODULE_NAME_UART2 && time == 0 && state <= 1);
  votes++;
  if (pm_error) return -1;
  awake = state == 0;
  return 0;
}
int bk_uart_take_rx_isr(uart_id_t id, void (*f)(uart_id_t, void*), void* u) {
  (void)id;
  (void)f;
  (void)u;
  return 0;
}
int bk_uart_enable_rx_interrupt(uart_id_t id) {
  (void)id;
  return 0;
}
int bk_uart_disable_rx_interrupt(uart_id_t id) {
  (void)id;
  return 0;
}
int bk_uart_recover_rx_isr(uart_id_t id) {
  (void)id;
  return 0;
}
int bk_uart_set_enable_tx(uart_id_t id, bool on) {
  (void)id;
  (void)on;
  return 0;
}
bool bk_uart_is_tx_over(uart_id_t id) {
  (void)id;
  return now_ms >= drain_until;
}
int uart_read_byte_ex(uart_id_t id, uint8_t* b) {
  (void)id;
  (void)b;
  return -1;
}
int uart_write_ready(uart_id_t id) {
  (void)id;
  return accepted < capacity ? 0 : -1;
}
int uart_write_byte(uart_id_t id, uint8_t b) {
  (void)id;
  size_t other_written = 99;
  assert(direct_write(NULL, "transport", 9, &other_written, 0) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(other_written == 0);
  ++competing_attempts;
  assert(suspended && awake);
  assert(accepted < capacity);
  if (byte_error) return -1;
  wire[wire_size++] = (char)b;
  accepted++;
  drain_until = now_ms + write_drain_ms;
  return 0;
}
static void reset(void) {
  wire_size = 0;
  competing_attempts = 0;
  memset(wire, 0, sizeof(wire));
  now_ms = lock_until = drain_until = 0;
  capacity = 20;
  accepted = suspended = resumes = byte_error = 0;
  write_drain_ms = 0;
  awake = 1;
  votes = pm_error = baud_error = resume_ms = 0;
  sdk_pending = sdk_outputs = 0;
  s_direct_initialized = 1;
  s_direct_releasing = 0;
  s_direct_release_steps = 0;
  s_direct_tx_busy = 0;
}

#define main launcher_main
#include "projects/testing/targets/h2_loader_tar_zlib/unit/pal-disk/beken/bk7258_v3_202405/ap/main.c"
#undef main

void bk_init(void) {}
int h2_bk_target_task_policy_install(void) { return 0; }
int rtos_get_core_id(void) { return 1; }
int rtos_core0_create_thread(beken_thread_t* thread, int priority,
                             const char* name, void (*entry)(void*),
                             unsigned stack, void* user) {
  (void)thread;
  (void)priority;
  (void)name;
  (void)stack;
  (void)user;
  assert(entry == controller && s_direct_initialized && wire_size > 0);
  return 0;
}
int h2_bk_pal_disk_management_start(void) { return H2_PAL_ERR_IO; }
const h2_pal_disk_api_t* h2_bk_pal_disk_disk(void) { return NULL; }
const h2_pal_task_api_t* h2_bk_platform_task_api(void) { return NULL; }
const h2_pal_time_api_t* h2_bk_platform_time_api(void) { return NULL; }
h2_pal_mem_api_t* h2_bk_platform_sram_allocator(void) { return NULL; }

int main(void) {
  reset();
  capacity = sizeof(wire);
  s_direct_initialized = 0;
  awake = 0;
  assert(launcher_main() == 0 && awake && votes == 1);
  assert(strstr(wire, "phase=boot"));
  reset();
  s_direct_initialized = 0;
  pm_error = 1;
  assert(launcher_main() == -1 && wire_size == 0);
  h2_pal_disk_e2e_result_t result = {0};
  reset();
  capacity = sizeof(wire);
  sdk_pending = 1;
  write_drain_ms = 3;
  assert(emit(NULL, "boot", &result) == 0);
  assert(strstr(wire, "H2_PAL_DISK_E2E contract=") &&
         strstr(wire, "phase=boot"));
  assert(competing_attempts == accepted && sdk_outputs == 1 && resumes == 1);
  assert(memcmp(wire + accepted - 2, "\r\nSDK", 5) == 0);
  size_t written = 0;
  assert(direct_write(NULL, "transport", 9, &written, 1000) == 0 &&
         written == 9);
  assert(memcmp(wire + wire_size - 9, "transport", 9) == 0);
  reset();
  capacity = sizeof(wire);
  result.current_case = 0;
  result.cases[0].status = H2_PAL_DISK_E2E_PASS;
  assert(emit(NULL, "case", &result) == 0);
  assert(strstr(wire, "H2_PAL_DISK_CASE id=") &&
         strstr(wire, "status=PASS rc=0"));
  result.cases[0].status = (h2_pal_disk_e2e_status_t)-1;
  assert(emit(NULL, "case", &result) == H2_PAL_ERR_INVALID_ARG);
  reset();
  capacity = 3;
  assert(emit(NULL, "boot", &result) == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 3 && now_ms == 1000 && resumes == 1 && !s_direct_tx_busy);
  reset();
  byte_error = 1;
  assert(emit(NULL, "boot", &result) == H2_PAL_ERR_IO);
  assert(!accepted && resumes == 1);
  reset();
  lock_until = 2000;
  assert(emit(NULL, "boot", &result) == H2_PAL_ERR_TIMEOUT);
  assert(!accepted && now_ms == 1000 && !resumes);
  reset();
  s_direct_initialized = 0;
  assert(emit(NULL, "boot", &result) == H2_PAL_ERR_CLOSED);
  return 0;
}
