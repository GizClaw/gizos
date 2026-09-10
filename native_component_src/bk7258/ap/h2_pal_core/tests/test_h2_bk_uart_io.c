#include <assert.h>
#include <stddef.h>
#define H2_BK_UART_IO_TEST 1
#include "h2_bk_platform_uart_io_stream.c"
static uint32_t now_ms, lock_until, drain_until;
static int capacity, accepted, suspended, resumes, byte_error;
static int control(shell_dev_t *dev, int cmd, void *arg) {
  (void)dev; (void)arg;
  if (cmd == SHELL_IO_CTRL_TX_SUSPEND) { assert(now_ms >= drain_until); suspended = 1; }
  else { assert(suspended); suspended = 0; resumes++; }
  return 1;
}
static shell_drv_t driver = {control};
shell_dev_t shell_uart = {&driver};
int rtos_init_mutex(beken_mutex_t *m) { *m=0; return 0; }
int rtos_deinit_mutex(beken_mutex_t *m) { (void)m; return 0; }
int rtos_trylock_mutex(beken_mutex_t *m) { if (now_ms < lock_until || *m) return -1; *m=1; return 0; }
int rtos_unlock_mutex(beken_mutex_t *m) { assert(*m); *m=0; return 0; }
uint32_t rtos_get_time(void) { return now_ms; }
uint32_t rtos_enter_critical(void) { return 0; }
void rtos_exit_critical(uint32_t l) { (void)l; }
void rtos_delay_milliseconds(uint32_t n) { now_ms += n; }
int bk_uart_set_baud_rate(uart_id_t id,uint32_t n) { (void)id;(void)n;return 0; }
int bk_uart_take_rx_isr(uart_id_t id,void (*f)(uart_id_t,void *),void *u) { (void)id;(void)f;(void)u;return 0; }
int bk_uart_enable_rx_interrupt(uart_id_t id) { (void)id;return 0; }
int bk_uart_disable_rx_interrupt(uart_id_t id) { (void)id;return 0; }
int bk_uart_recover_rx_isr(uart_id_t id) { (void)id;return 0; }
int bk_uart_set_enable_tx(uart_id_t id,bool on) { (void)id;(void)on;return 0; }
bool bk_uart_is_tx_over(uart_id_t id) { (void)id;return now_ms >= drain_until; }
int uart_read_byte_ex(uart_id_t id,uint8_t *b) { (void)id;(void)b;return -1; }
int uart_write_ready(uart_id_t id) { (void)id;return accepted < capacity ? 0 : -1; }
int uart_write_byte(uart_id_t id,uint8_t b) { (void)id;(void)b;assert(suspended);assert(accepted < capacity);if(byte_error)return -1;accepted++;return 0; }
static void reset(void) {
  now_ms=lock_until=drain_until=0;capacity=20;accepted=suspended=resumes=byte_error=0;
  s_direct_initialized=1;s_direct_write_mutex=0;
}
static int write_test(uint32_t timeout,size_t *n) { return direct_write(NULL,"12345678",8,n,timeout); }
int main(void) {
  size_t n;
  reset();lock_until=100;
  assert(write_test(5,&n)==H2_PAL_ERR_TIMEOUT && now_ms==5 && n==0 && !suspended);
  reset();lock_until=100;
  assert(write_test(0,&n)==H2_PAL_ERR_WOULD_BLOCK && now_ms==0 && n==0);
  reset();drain_until=100;
  assert(write_test(5,&n)==H2_PAL_ERR_TIMEOUT && now_ms==5 && !s_direct_write_mutex && !resumes);
  reset();lock_until=3;drain_until=100;
  assert(write_test(5,&n)==H2_PAL_ERR_TIMEOUT && now_ms==5);
  reset();capacity=0;
  assert(write_test(5,&n)==H2_PAL_ERR_TIMEOUT && now_ms==5 && n==0 && resumes==1 && !suspended);
  reset();capacity=0;
  assert(write_test(0,&n)==H2_PAL_ERR_WOULD_BLOCK && now_ms==0 && resumes==1);
  reset();capacity=3;
  assert(write_test(0,&n)==H2_PAL_OK && n==3 && now_ms==0 && resumes==1);
  reset();capacity=3;
  assert(write_test(5,&n)==H2_PAL_ERR_TIMEOUT && n==3 && now_ms==5 && resumes==1);
  reset();byte_error=1;
  assert(write_test(5,&n)==H2_PAL_ERR_IO && n==0 && resumes==1);
  reset();assert(write_test(5,&n)==H2_PAL_OK && n==8 && resumes==1 && !s_direct_write_mutex);
  return 0;
}
