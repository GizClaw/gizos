#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "h2_bk_platform_core.h"
#define H2_BK_UART_IO_TEST 1
#include "h2_bk_platform_uart_io_stream.c"
static uint32_t now_ms, lock_until, drain_until;
static int capacity, accepted, suspended, resumes, byte_error;
static uint32_t write_drain_ms;
static int awake, votes, pm_error, pm_release_error, baud_error, resume_ms;
static int sdk_pending, sdk_outputs;
static unsigned rx_disables, rx_restores, configures, rx_takes;
static int disable_error, restore_error, lifecycle_probe;
static int async_pool_full, async_calls, async_drops, enable_tx_error;
static int suspend_error, resume_error, byte_error_after;
static unsigned char transmitted[2048];
int h2_test_bk_async_printf(const char* format, ...) {
  char record[2048];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(record, sizeof(record), format, args);
  va_end(args);
  ++async_calls;
  if (async_pool_full) ++async_drops;
  return length;
}
static int control(shell_dev_t* dev, int cmd, void* arg) {
  (void)dev;
  (void)arg;
  if (cmd == SHELL_IO_CTRL_TX_SUSPEND) {
    assert(now_ms >= drain_until);
    if (suspend_error) return 0;
    suspended = 1;
  } else {
    assert(suspended);
    if (sdk_pending) {
      /* The queued SDK producer may resume only after direct FIFO data. */
      assert(now_ms >= drain_until);
      sdk_pending = 0;
      sdk_outputs++;
    }
    suspended = 0;
    resumes++;
    resume_ms = (int)now_ms;
    if (resume_error) return 0;
  }
  return 1;
}
static shell_drv_t driver = {control};
shell_dev_t shell_uart = {&driver};
uint32_t rtos_get_time(void) { return now_ms; }
uint32_t rtos_enter_critical(void) {
  /* Model another task retaining the production admission gate. */
  if (lock_until != 0u) {
    s_direct_tx_busy = now_ms < lock_until;
    if (!s_direct_tx_busy) lock_until = 0u;
  }
  return 0;
}
void rtos_exit_critical(uint32_t l) { (void)l; }
void rtos_delay_milliseconds(uint32_t n) { now_ms += n; }
int bk_uart_set_baud_rate(uart_id_t id, uint32_t n) {
  (void)id;
  (void)n;
  ++configures;
  return baud_error;
}
int bk_pm_module_vote_sleep_ctrl(pm_sleep_module_name_e module, uint32_t state,
                                 uint32_t time) {
  assert(module == PM_SLEEP_MODULE_NAME_UART2 && time == 0 && state <= 1);
  votes++;
  if (pm_error || (state == 1u && pm_release_error)) return -1;
  awake = state == 0;
  return 0;
}
int bk_uart_take_rx_isr(uart_id_t id, void (*f)(uart_id_t, void*), void* u) {
  (void)id;
  (void)f;
  (void)u;
  ++rx_takes;
  return 0;
}
int bk_uart_enable_rx_interrupt(uart_id_t id) {
  (void)id;
  return 0;
}
int bk_uart_disable_rx_interrupt(uart_id_t id) {
  (void)id;
  ++rx_disables;
  return disable_error;
}
int bk_uart_recover_rx_isr(uart_id_t id) {
  (void)id;
  ++rx_restores;
  return restore_error;
}
int bk_uart_set_enable_tx(uart_id_t id, bool on) {
  (void)id;
  (void)on;
  return enable_tx_error;
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
  assert(suspended && awake && s_direct_tx_busy);
  assert(accepted < capacity);
  if (lifecycle_probe) {
    lifecycle_probe = 0;
    const h2_pal_io_stream_api_t* stream = h2_bk_platform_io_stream_api();
    assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_BUSY);
    assert(h2_pal_io_stream_release(stream) == H2_PAL_ERR_BUSY);
    assert(!rx_takes && !rx_disables && !rx_restores && !configures);
  }
  if (byte_error || accepted == byte_error_after) return -1;
  assert((size_t)accepted < sizeof(transmitted));
  transmitted[accepted++] = b;
  drain_until = now_ms + write_drain_ms;
  return 0;
}
static void reset(void) {
  now_ms = lock_until = drain_until = 0;
  capacity = 20;
  accepted = suspended = resumes = byte_error = 0;
  write_drain_ms = 0;
  awake = 1;
  votes = pm_error = pm_release_error = baud_error = resume_ms = 0;
  sdk_pending = sdk_outputs = 0;
  rx_disables = rx_restores = configures = rx_takes = 0;
  disable_error = restore_error = lifecycle_probe = 0;
  async_pool_full = async_calls = async_drops = enable_tx_error = 0;
  suspend_error = resume_error = 0;
  byte_error_after = -1;
  memset(transmitted, 0, sizeof(transmitted));
  s_direct_initialized = 1;
  s_direct_releasing = 0;
  s_direct_release_steps = 0u;
  s_direct_tx_busy = 0;
}
static int write_test(uint32_t timeout, size_t* n) {
  return direct_write(NULL, "12345678", 8, n, timeout);
}
static int console_log(const char* message) {
  return h2_pal_log_write(h2_bk_platform_console_log_api(), H2_PAL_LOG_INFO,
                          "ignored", message);
}
static void test_console_log(void) {
  char record[600];
  memset(record, 'r', sizeof(record) - 3u);
  memcpy(record + sizeof(record) - 3u, "\r\n", 3u);
  reset();
  async_pool_full = 1;
  assert(h2_test_bk_async_printf("%s", record) == (int)strlen(record));
  assert(async_calls == 1 && async_drops == 1 && accepted == 0);
  capacity = sizeof(transmitted);
  sdk_pending = 1;
  write_drain_ms = 2u;
  lifecycle_probe = 1;
  s_direct_head = 7u;
  s_direct_tail = 3u;
  s_direct_overflow = 1;
  assert(console_log(record) == H2_PAL_OK);
  assert((size_t)accepted == strlen(record) &&
         memcmp(transmitted, record, strlen(record)) == 0);
  assert(async_calls == 1 && async_drops == 1 && sdk_outputs == 1);
  assert(!s_direct_tx_busy && !suspended && resumes == 1 && votes == 0);
  assert(s_direct_head == 7u && s_direct_tail == 3u && s_direct_overflow);

  reset();
  capacity = 3;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 3 && now_ms == 5000u && resumes == 1 &&
         memcmp(transmitted, "rec", 3u) == 0 && !s_direct_tx_busy);
  reset();
  capacity = 0;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 0 && now_ms == 5000u && resumes == 1);
  reset();
  lock_until = 6000u;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 0 && now_ms == 5000u && !resumes);
  reset();
  lock_until = 5000u;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 0 && now_ms == 5000u && !resumes && !s_direct_tx_busy);
  reset();
  drain_until = 5000u;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 0 && now_ms == 5000u && !resumes && !s_direct_tx_busy);
  reset();
  byte_error_after = 3;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(accepted == 3 && resumes == 1 && !s_direct_tx_busy);
  reset();
  write_drain_ms = 5001u;
  assert(console_log("record\r\n") == H2_PAL_ERR_TIMEOUT);
  assert(accepted == 8 && now_ms == 5000u && resumes == 1);
  reset();
  suspend_error = 1;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(!accepted && !resumes && !s_direct_tx_busy);
  reset();
  enable_tx_error = -1;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(!accepted && resumes == 1 && !s_direct_tx_busy);
  reset();
  resume_error = 1;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(accepted == 8 && !s_direct_tx_busy);

  reset();
  s_direct_initialized = 0;
  awake = 0;
  lifecycle_probe = 1;
  assert(console_log("record\r\n") == H2_PAL_OK);
  assert(!s_direct_initialized && !awake && votes == 2);
  assert(!configures && !rx_takes && !rx_disables && !rx_restores);
  reset();
  s_direct_releasing = 1;
  assert(console_log("record\r\n") == H2_PAL_ERR_CLOSED);
  assert(!accepted && !resumes && !votes && !s_direct_tx_busy);
  reset();
  s_direct_initialized = 0;
  pm_error = -1;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(!accepted && !resumes && votes == 1 && !s_direct_tx_busy);
  reset();
  s_direct_initialized = 0;
  pm_release_error = -1;
  assert(console_log("record\r\n") == H2_PAL_ERR_IO);
  assert(accepted == 8 && votes == 2 && awake && !s_direct_tx_busy);
  reset();
  assert(console_log(NULL) == H2_PAL_ERR_INVALID_ARG);
  assert(h2_pal_log_write(h2_bk_platform_console_log_api(),
                          (h2_pal_log_level_t)99, NULL,
                          "record") == H2_PAL_ERR_INVALID_ARG);
  assert(!accepted && !votes);
}
int main(void) {
  test_console_log();
  size_t n;
  reset();
  lock_until = 100;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && now_ms == 5 && n == 0 &&
         !suspended);
  reset();
  lock_until = 100;
  assert(write_test(0, &n) == H2_PAL_ERR_WOULD_BLOCK && now_ms == 0 && n == 0);
  reset();
  drain_until = 100;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && now_ms == 5 &&
         !s_direct_tx_busy && !resumes);
  reset();
  lock_until = 3;
  drain_until = 100;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && now_ms == 5);
  reset();
  capacity = 0;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && now_ms == 5 && n == 0 &&
         resumes == 1 && !suspended);
  reset();
  capacity = 0;
  assert(write_test(0, &n) == H2_PAL_ERR_WOULD_BLOCK && now_ms == 0 &&
         resumes == 1);
  reset();
  capacity = 3;
  assert(write_test(0, &n) == H2_PAL_OK && n == 3 && now_ms == 0 &&
         resumes == 1);
  reset();
  capacity = 3;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && n == 3 && now_ms == 5 &&
         resumes == 1);
  reset();
  byte_error = 1;
  assert(write_test(5, &n) == H2_PAL_ERR_IO && n == 0 && resumes == 1);
  reset();
  assert(write_test(5, &n) == H2_PAL_OK && n == 8 && resumes == 1 &&
         !s_direct_tx_busy);
  reset();
  write_drain_ms = 3;
  assert(write_test(5, &n) == H2_PAL_OK && n == 8 && now_ms == 3 &&
         resume_ms == 3 && resumes == 1);
  reset();
  write_drain_ms = 3;
  sdk_pending = 1;
  assert(write_test(5, &n) == H2_PAL_OK && n == 8 && sdk_outputs == 1 &&
         !sdk_pending && resumes == 1);
  reset();
  write_drain_ms = 6;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && n == 8 && now_ms == 5 &&
         resumes == 1 && !s_direct_tx_busy);
  reset();
  lock_until = 3;
  write_drain_ms = 3;
  assert(write_test(5, &n) == H2_PAL_ERR_TIMEOUT && n == 8 && now_ms == 5 &&
         resumes == 1);
  reset();
  write_drain_ms = 1;
  assert(write_test(0, &n) == H2_PAL_ERR_WOULD_BLOCK && n == 8 && now_ms == 0 &&
         resumes == 1);
  const h2_pal_uart_io_stream_config_t config = {
      .baud_rate = 460800,
      .data_bits = 8,
      .stop_bits = 1,
      .parity = H2_PAL_UART_PARITY_NONE,
      .flow_control = H2_PAL_UART_FLOW_CONTROL_NONE};
  reset();
  s_direct_initialized = 0;
  awake = 0;
  assert(direct_configure(NULL, &config) == H2_PAL_OK && awake && votes == 1);
  assert(direct_configure(NULL, &config) == H2_PAL_OK && votes == 1);
  write_drain_ms = 3;
  assert(write_test(5, &n) == H2_PAL_OK && awake && now_ms == 3);
  const h2_pal_uart_io_stream_api_t* api = h2_bk_platform_uart_io_stream_api();
  assert(h2_pal_uart_io_stream_release(api) == H2_PAL_OK);
  assert(!s_direct_initialized && !awake && votes == 2 && rx_disables == 1 &&
         rx_restores == 1 && !s_direct_tx_busy);
  assert(h2_pal_uart_io_stream_release(api) == H2_PAL_OK);
  assert(votes == 2 && rx_disables == 1 && rx_restores == 1);
  assert(write_test(5, &n) == H2_PAL_ERR_CLOSED && n == 0);
  reset();
  s_direct_initialized = 0;
  awake = 0;
  pm_error = 1;
  assert(direct_configure(NULL, &config) == H2_PAL_ERR_IO &&
         !s_direct_initialized && !awake && votes == 1);
  reset();
  s_direct_initialized = 0;
  awake = 0;
  baud_error = -1;
  assert(direct_configure(NULL, &config) == H2_PAL_ERR_IO &&
         !s_direct_initialized && !awake && votes == 2);
  reset();
  s_direct_initialized = 0;
  awake = 0;
  const h2_pal_io_stream_api_t* stream = h2_bk_platform_io_stream_api();
  assert(!s_direct_initialized && votes == 0);
  baud_error = -1;
  assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_IO);
  assert(!s_direct_initialized && !awake);
  baud_error = 0;
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK);
  int start_votes = votes;
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK && votes == start_votes);
  sdk_pending = 1;
  write_drain_ms = 2;
  assert(h2_pal_io_stream_write(stream, "frame", 5u, &n, 10u) == H2_PAL_OK);
  assert(n == 5u && sdk_outputs == 1 && !suspended);
  capacity = accepted + 2;
  write_drain_ms = 0;
  assert(h2_pal_io_stream_write(stream, "frame", 5u, &n, 0u) ==
         H2_PAL_ERR_WOULD_BLOCK);
  assert(n == 2u && !suspended);
  assert(h2_pal_io_stream_flush(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(!s_direct_initialized && !awake);
  assert(h2_pal_io_stream_read(stream, &n, sizeof(n), &n, 0u) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  for (unsigned failure = 0u; failure < 3u; ++failure) {
    reset();
    int* error = failure == 0u   ? &disable_error
                 : failure == 1u ? &restore_error
                                 : &pm_error;
    *error = -1;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_ERR_IO);
    assert(s_direct_initialized && s_direct_releasing);
    assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_io_stream_write(stream, "f", 1u, &n, 0u) ==
           H2_PAL_ERR_CLOSED);
    assert(n == 0u);
    *error = 0;
    unsigned saved_disables = rx_disables, saved_restores = rx_restores;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
    assert(!s_direct_initialized && !s_direct_releasing && !awake);
    assert(rx_disables == saved_disables + (failure == 0u ? 1u : 0u));
    assert(rx_restores == saved_restores + (failure <= 1u ? 1u : 0u));
    saved_disables = rx_disables;
    saved_restores = rx_restores;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
    assert(rx_disables == saved_disables && rx_restores == saved_restores);
  }
  return 0;
}
