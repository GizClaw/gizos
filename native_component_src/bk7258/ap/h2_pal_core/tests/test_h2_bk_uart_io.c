#include <assert.h>
#include <stddef.h>
#define H2_BK_UART_IO_TEST 1
#include "h2_bk_platform_uart_io_stream.c"
static uint32_t now_ms, lock_until, drain_until;
static int capacity, accepted, suspended, resumes, byte_error;
static uint32_t write_drain_ms;
static int awake, votes, pm_error, baud_error, resume_ms;
static int sdk_pending, sdk_outputs;
static unsigned rx_disables, rx_restores, mutex_releases;
static int disable_error, restore_error, mutex_release_error;
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
int rtos_init_mutex(beken_mutex_t* m) {
  *m = 0;
  return 0;
}
int rtos_deinit_mutex(beken_mutex_t* m) {
  (void)m;
  ++mutex_releases;
  return mutex_release_error;
}
int rtos_trylock_mutex(beken_mutex_t* m) {
  if (now_ms < lock_until || *m) return -1;
  *m = 1;
  return 0;
}
int rtos_unlock_mutex(beken_mutex_t* m) {
  assert(*m);
  *m = 0;
  return 0;
}
uint32_t rtos_get_time(void) { return now_ms; }
uint32_t rtos_enter_critical(void) { return 0; }
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
  (void)b;
  assert(suspended && awake);
  assert(accepted < capacity);
  if (byte_error) return -1;
  accepted++;
  drain_until = now_ms + write_drain_ms;
  return 0;
}
static void reset(void) {
  now_ms = lock_until = drain_until = 0;
  capacity = 20;
  accepted = suspended = resumes = byte_error = 0;
  write_drain_ms = 0;
  awake = 1;
  votes = pm_error = baud_error = resume_ms = 0;
  sdk_pending = sdk_outputs = 0;
  rx_disables = rx_restores = mutex_releases = 0;
  disable_error = restore_error = mutex_release_error = 0;
  s_direct_initialized = 1;
  s_direct_releasing = 0;
  s_direct_release_steps = 0u;
  s_direct_write_mutex = 0;
}
static int write_test(uint32_t timeout, size_t* n) {
  return direct_write(NULL, "12345678", 8, n, timeout);
}
int main(void) {
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
         !s_direct_write_mutex && !resumes);
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
         !s_direct_write_mutex);
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
         resumes == 1 && !s_direct_write_mutex);
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
         rx_restores == 1 && mutex_releases == 1);
  assert(h2_pal_uart_io_stream_release(api) == H2_PAL_OK);
  assert(votes == 2 && rx_disables == 1 && rx_restores == 1 &&
         mutex_releases == 1);
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
  for (unsigned failure = 0u; failure < 4u; ++failure) {
    reset();
    int* error = failure == 0u   ? &disable_error
                 : failure == 1u ? &restore_error
                 : failure == 2u ? &mutex_release_error
                                 : &pm_error;
    *error = -1;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_ERR_IO);
    assert(s_direct_initialized && s_direct_releasing);
    assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_INVALID_STATE);
    assert(h2_pal_io_stream_write(stream, "f", 1u, &n, 0u) ==
           H2_PAL_ERR_CLOSED);
    assert(n == 0u);
    *error = 0;
    unsigned saved_mutex_releases = mutex_releases;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
    assert(!s_direct_initialized && !s_direct_releasing && !awake);
    assert(mutex_releases == saved_mutex_releases + (failure < 3u ? 1u : 0u));
    saved_mutex_releases = mutex_releases;
    assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
    assert(mutex_releases == saved_mutex_releases);
  }
  return 0;
}
