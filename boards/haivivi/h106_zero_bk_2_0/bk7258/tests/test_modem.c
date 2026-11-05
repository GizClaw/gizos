#include "h2_zero_bk_modem_test_sdk.h"
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

#include "../ap/src/h2_bk7258_board_modem.c"

static ppp_pcb pcb;
static int close_error, free_error, join_error, write_error;
static int close_calls, free_calls, join_calls, at_calls, deinit_calls;
static int sem_count;
static bool terminal_on_close = true;
static bool peer_on_wait;
static bool connected_on_wait;
static uint32_t clock_ms;
static h2_pal_result_t base_error;
static bool raw_owned;
static h2_pal_result_t drop_result;
static pthread_mutex_t operation_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t control_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_cond = PTHREAD_COND_INITIALIZER;
static bool pause_join, join_entered, release_join, query_attempted;
static pthread_t query_thread;
static _Thread_local bool is_query_thread;
static h2_pal_result_t lock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  if (is_query_thread && (void*)mutex == &operation_mutex) {
    pthread_mutex_lock(&control_mutex);
    query_attempted = true;
    pthread_cond_broadcast(&control_cond);
    pthread_mutex_unlock(&control_mutex);
  }
  return pthread_mutex_lock((pthread_mutex_t*)mutex) == 0 ? H2_PAL_OK
                                                          : H2_PAL_ERR_IO;
}
static h2_pal_result_t unlock_mutex(void* user, h2_pal_mutex_t* mutex) {
  (void)user;
  return pthread_mutex_unlock((pthread_mutex_t*)mutex) == 0 ? H2_PAL_OK
                                                            : H2_PAL_ERR_IO;
}
static const h2_pal_sync_vtable_t sync_vtable = {.lock_mutex = lock_mutex,
                                                 .unlock_mutex = unlock_mutex};
static const h2_pal_sync_api_t sync_api = {.vtable = &sync_vtable};

int gpio_dev_unmap(gpio_id_t g) {
  (void)g;
  return BK_OK;
}
int gpio_dev_map(gpio_id_t g, int f) {
  (void)g;
  (void)f;
  return BK_OK;
}
int bk_gpio_set_config(gpio_id_t g, const gpio_config_t* c) {
  (void)g;
  (void)c;
  return BK_OK;
}
int bk_gpio_set_output_high(gpio_id_t g) {
  (void)g;
  return BK_OK;
}
int bk_gpio_set_output_low(gpio_id_t g) {
  (void)g;
  return BK_OK;
}
int bk_gpio_pull_up(gpio_id_t g) {
  (void)g;
  return BK_OK;
}
uint32_t bk_gpio_get_value(gpio_id_t g) {
  (void)g;
  return 0;
}
int bk_uart_init(uart_id_t id, const uart_config_t* c) {
  (void)id;
  (void)c;
  return BK_OK;
}
int bk_uart_deinit(uart_id_t id) {
  (void)id;
  assert(s_ppp_rx_thread == NULL);
  deinit_calls++;
  return BK_OK;
}
int bk_uart_set_enable_rx(uart_id_t id, bool enabled) {
  (void)id;
  (void)enabled;
  return BK_OK;
}
int bk_uart_set_enable_tx(uart_id_t id, bool enabled) {
  (void)id;
  (void)enabled;
  return BK_OK;
}
int bk_uart_set_rx_timeout(uart_id_t id, uint32_t n) {
  (void)id;
  (void)n;
  return BK_OK;
}
int bk_uart_enable_rx_interrupt(uart_id_t id) {
  (void)id;
  return BK_OK;
}
uint32_t uart_get_length_in_buffer(uart_id_t id) {
  (void)id;
  return 0;
}
int bk_uart_read_bytes(uart_id_t id, void* p, uint32_t n, uint32_t ms) {
  (void)id;
  (void)p;
  (void)n;
  (void)ms;
  return BK_ERR_UART_RX_TIMEOUT;
}
int bk_uart_write_bytes(uart_id_t id, const void* p, uint32_t n) {
  (void)id;
  (void)p;
  (void)n;
  assert(s_ppp_rx_thread == NULL);
  return write_error;
}
void bk_uart_wait_tx_over(uart_id_t id) { (void)id; }
void rtos_delay_milliseconds(uint32_t n) { clock_ms += n; }
uint32_t rtos_get_time(void) { return clock_ms; }
int rtos_init_semaphore(beken_semaphore_t* s, int n) {
  (void)n;
  *s = (void*)1;
  return kNoErr;
}
int rtos_deinit_semaphore(beken_semaphore_t* s) {
  *s = NULL;
  return kNoErr;
}
int rtos_set_semaphore(beken_semaphore_t* s) {
  (void)s;
  sem_count = 1;
  return kNoErr;
}
int rtos_get_semaphore(beken_semaphore_t* s, uint32_t ms) {
  (void)s;
  if (ms && peer_on_wait) {
    peer_on_wait = false;
    modem_ppp_status(s_ppp, 1, NULL);
  }
  if (ms && connected_on_wait) {
    connected_on_wait = false;
    s_ppp_netif.ip.addr = 0x01020304;
    modem_ppp_status(s_ppp, PPPERR_NONE, NULL);
  }
  if (!sem_count) return -1;
  sem_count = 0;
  return kNoErr;
}
int rtos_thread_join(beken_thread_t* t) {
  join_calls++;
  if (pause_join) {
    pthread_mutex_lock(&control_mutex);
    join_entered = true;
    pthread_cond_broadcast(&control_cond);
    while (!release_join) pthread_cond_wait(&control_cond, &control_mutex);
    pthread_mutex_unlock(&control_mutex);
  }
  if (join_error) return join_error;
  *t = NULL;
  return kNoErr;
}
int rtos_delete_thread(beken_thread_t* t) {
  (void)t;
  return kNoErr;
}
int rtos_core0_create_psram_thread(beken_thread_t* t, uint8_t priority,
                                   const char* name, void (*f)(void*),
                                   uint32_t size, void* arg) {
  (void)priority;
  (void)name;
  (void)f;
  (void)size;
  (void)arg;
  assert(s_ppp != NULL);
  *t = (void*)2;
  return kNoErr;
}
ppp_pcb* pppapi_pppos_create(struct netif* n,
                             u32_t (*out)(ppp_pcb*, u8_t*, u32_t, void*),
                             void (*status)(ppp_pcb*, int, void*), void* ctx) {
  (void)n;
  (void)out;
  (void)status;
  (void)ctx;
  assert(s_ppp == NULL && s_ppp_rx_thread == NULL);
  return &pcb;
}
err_t pppapi_close(ppp_pcb* p, u8_t n) {
  (void)p;
  (void)n;
  close_calls++;
  if (close_error) return close_error;
  if (terminal_on_close) modem_ppp_status(p, 1, NULL);
  return ERR_OK;
}
err_t pppapi_free(ppp_pcb* p) {
  (void)p;
  free_calls++;
  assert(s_ppp_rx_thread == NULL);
  assert(!s_ppp_connect_started || s_ppp_terminal);
  return free_error;
}
err_t pppapi_connect(ppp_pcb* p, uint16_t n) {
  (void)p;
  (void)n;
  return ERR_OK;
}
err_t pppos_input_tcpip(ppp_pcb* p, u8_t* data, int n) {
  (void)p;
  (void)data;
  (void)n;
  return ERR_OK;
}
void ppp_set_usepeerdns(ppp_pcb* p, u8_t b) {
  (void)p;
  (void)b;
}
void ppp_set_auth(ppp_pcb* p, u8_t b, const char* u, const char* pass) {
  (void)p;
  (void)b;
  (void)u;
  (void)pass;
}
const ip_addr_t* dns_getserver(u8_t n) {
  (void)n;
  return NULL;
}
void h2_bk_platform_netif_register(struct netif* n, h2_pal_netif_kind_t k) {
  (void)n;
  (void)k;
}
void h2_bk_platform_netif_unregister(struct netif* n) { (void)n; }
h2_pal_result_t h2_bk_platform_netif_reconcile_default_async(void) {
  return H2_PAL_OK;
}
h2_pal_result_t h2_zero_bk_peripheral_power_enable(void) { return H2_PAL_OK; }
const h2_pal_sync_api_t* h2_bk7258_board_sync_api(void) { return NULL; }
h2_pal_mem_api_t* h2_bk7258_board_default_allocator(void) { return NULL; }
const h2_pal_system_event_api_t* h2_bk7258_board_system_event_api(void) {
  return NULL;
}
const char* h2_apn_from_imsi(const char* imsi) {
  (void)imsi;
  return "test.invalid";
}

h2_pal_result_t h2_quectel_modem_transport_closing(h2_quectel_modem_t* m) {
  return m->ota_hold ? H2_PAL_ERR_BUSY : H2_PAL_OK;
}
h2_pal_result_t h2_quectel_modem_set_raw_ppp_owned(h2_quectel_modem_t* m,
                                                   int owned) {
  if (owned && m->ota_hold) return H2_PAL_ERR_BUSY;
  raw_owned = owned != 0;
  return H2_PAL_OK;
}
h2_pal_result_t h2_quectel_modem_dial_ppp(h2_quectel_modem_t* m) {
  (void)m;
  return H2_PAL_OK;
}
h2_pal_result_t h2_quectel_modem_drop_ppp(h2_quectel_modem_t* m) {
  (void)m;
  assert(!raw_owned && s_ppp_rx_thread == NULL);
  return drop_result;
}
h2_pal_result_t h2_quectel_modem_set_apn(h2_pal_modem_t* m,
                                         const h2_pal_modem_apn_config_t* c) {
  (void)m;
  (void)c;
  return H2_PAL_OK;
}
h2_pal_result_t h2_quectel_modem_init(h2_quectel_modem_t* m,
                                      const h2_quectel_modem_config_t* c) {
  (void)m;
  (void)c;
  return H2_PAL_OK;
}
h2_pal_modem_t* h2_quectel_modem_platform(h2_quectel_modem_t* m) {
  return &m->platform;
}

static h2_pal_result_t base_open(void* user, uint32_t ms) {
  (void)user;
  (void)ms;
  assert(!modem_ppp_retained());
  at_calls++;
  return base_error;
}
static h2_pal_result_t base_close(void* user, uint32_t ms) {
  (void)user;
  (void)ms;
  assert(!modem_ppp_retained());
  at_calls++;
  if (base_error) return base_error;
  return modem_transport_deinit(NULL);
}
static h2_pal_result_t base_identity(void* user, h2_pal_modem_identity_t* out) {
  (void)user;
  (void)out;
  assert(!modem_ppp_retained());
  at_calls++;
  return base_error;
}
static h2_pal_result_t base_status(void* user, h2_pal_modem_status_t* out) {
  (void)user;
  assert(!modem_ppp_retained());
  at_calls++;
  out->registration = H2_PAL_MODEM_REGISTRATION_HOME;
  return base_error;
}
static h2_pal_result_t base_operator(void* user, h2_pal_modem_operator_t* out) {
  (void)user;
  (void)out;
  assert(!modem_ppp_retained());
  at_calls++;
  return base_error;
}
static h2_pal_result_t base_signal(void* user, h2_pal_modem_signal_t* out) {
  (void)user;
  (void)out;
  assert(!modem_ppp_retained());
  at_calls++;
  return base_error;
}
static const h2_pal_modem_vtable_t base = {.open = base_open,
                                           .close = base_close,
                                           .get_identity = base_identity,
                                           .get_status = base_status,
                                           .get_operator = base_operator,
                                           .get_signal = base_signal};

static void reset(void) {
  memset(&s_modem, 0, sizeof(s_modem));
  s_modem.platform.user = &s_modem;
  s_modem.config.command_timeout_ms = 1000;
  strcpy(s_modem.last_apn, "test.invalid");
  s_quectel_vtable = &base;
  s_ppp = NULL;
  s_ppp_rx_thread = NULL;
  s_ppp_event = (void*)1;
  s_ppp_terminal = false;
  s_ppp_connect_started = false;
  s_ppp_close_sent = false;
  s_ppp_close_requested = false;
  s_ppp_command_unconfirmed = false;
  s_ppp_rx_running = false;
  s_uart_open = true;
  close_error = free_error = join_error = write_error = 0;
  close_calls = free_calls = join_calls = at_calls = deinit_calls = sem_count =
      0;
  terminal_on_close = true;
  peer_on_wait = connected_on_wait = false;
  base_error = H2_PAL_OK;
  drop_result = H2_PAL_OK;
  raw_owned = false;
  pause_join = join_entered = release_join = query_attempted = false;
}
static void active(void) {
  reset();
  s_ppp = &pcb;
  s_ppp_rx_thread = (void*)2;
  s_ppp_rx_running = true;
  s_ppp_connect_started = true;
  s_ppp_command_unconfirmed = true;
  s_modem.data_status.state = H2_PAL_MODEM_DATA_OPEN;
  s_modem.data_status.ip4_valid = 1;
}
static void queries_blocked(void) {
  h2_pal_modem_identity_t id;
  h2_pal_modem_signal_t signal;
  assert(modem_get_identity_adapter(&s_modem, &id) == H2_PAL_ERR_BUSY);
  if (modem_data_snapshot().state != H2_PAL_MODEM_DATA_OPEN)
    assert(modem_get_signal_adapter(&s_modem, &signal) == H2_PAL_ERR_BUSY);
  assert(at_calls == 0);
}
static void test_peer_reopen(void) {
  active();
  modem_ppp_status(&pcb, 1, NULL);
  assert(s_ppp_terminal && s_ppp != NULL && s_ppp_rx_thread != NULL);
  assert(modem_transport_deinit(NULL) == H2_PAL_ERR_BUSY);
  assert(deinit_calls == 0);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_CLOSING);
  queries_blocked();
  join_error = -1;
  assert(modem_data_open(&s_modem.platform, 10) == H2_PAL_ERR_IO);
  assert(at_calls == 0 && s_ppp != NULL && s_ppp_rx_thread != NULL);
  join_error = 0;
  base_error = H2_PAL_ERR_TIMEOUT;
  assert(modem_data_open(&s_modem.platform, 10) == H2_PAL_ERR_TIMEOUT);
  assert(s_ppp == NULL && s_ppp_rx_thread == NULL && free_calls == 1);
}
static void test_close_timeout_retry(void) {
  active();
  terminal_on_close = false;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_TIMEOUT);
  assert(s_ppp && s_ppp_rx_thread && !deinit_calls && !free_calls);
  queries_blocked();
  peer_on_wait = true;
  assert(modem_data_close(&s_modem.platform, 10) == H2_PAL_OK);
  assert(close_calls == 1 && s_ppp == NULL && s_ppp_rx_thread == NULL);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_CLOSED);
}
static void test_link_up_is_not_terminal(void) {
  active();
  terminal_on_close = false;
  connected_on_wait = true;
  assert(modem_data_close_adapter(&s_modem, 10) == H2_PAL_ERR_TIMEOUT);
  assert(s_ppp != NULL && s_ppp_rx_thread != NULL && free_calls == 0);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_CLOSING);
  peer_on_wait = true;
  assert(modem_data_close_adapter(&s_modem, 10) == H2_PAL_OK);
}
static void test_failures_retry(void) {
  active();
  close_error = -1;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_IO);
  assert(!free_calls && !join_calls && !deinit_calls);
  close_error = 0;
  join_error = -1;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_IO);
  assert(s_ppp && s_ppp_rx_thread && !free_calls && !deinit_calls);
  join_error = 0;
  free_error = -1;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_IO);
  assert(s_ppp && !s_ppp_rx_thread && !deinit_calls);
  queries_blocked();
  free_error = 0;
  write_error = -1;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_IO);
  assert(!s_ppp && s_ppp_command_unconfirmed && !deinit_calls);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_CLOSING);
  write_error = 0;
  drop_result = H2_PAL_ERR_TIMEOUT;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_TIMEOUT);
  assert(s_ppp_command_unconfirmed && raw_owned && !deinit_calls);
  queries_blocked();
  drop_result = H2_PAL_OK;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_OK);
  assert(!s_ppp_command_unconfirmed && !raw_owned && deinit_calls == 1);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_CLOSED);
}
static void test_query_errors_and_ota(void) {
  reset();
  base_error = H2_PAL_ERR_TIMEOUT;
  h2_pal_modem_identity_t id;
  assert(modem_get_identity_adapter(&s_modem, &id) == H2_PAL_ERR_TIMEOUT);
  assert(at_calls == 1);
  active();
  s_modem.ota_hold = 1;
  assert(modem_close_adapter(&s_modem, 10) == H2_PAL_ERR_BUSY);
  assert(!close_calls && !join_calls && !deinit_calls);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_OPEN);
}
static void* close_thread_main(void* unused) {
  (void)unused;
  return (void*)(intptr_t)modem_close_adapter(&s_modem, 10);
}
static void* query_thread_main(void* unused) {
  (void)unused;
  h2_pal_modem_identity_t id;
  is_query_thread = true;
  return (void*)(intptr_t)modem_get_identity_adapter(&s_modem, &id);
}
static void test_query_waits_for_close(void) {
  active();
  s_modem.config.sync_api = &sync_api;
  s_modem.operation_lock = (h2_pal_mutex_t*)&operation_mutex;
  s_modem.lock = (h2_pal_mutex_t*)&state_mutex;
  pause_join = true;
  join_error = -1;
  pthread_t closer;
  assert(pthread_create(&closer, NULL, close_thread_main, NULL) == 0);
  pthread_mutex_lock(&control_mutex);
  while (!join_entered) pthread_cond_wait(&control_cond, &control_mutex);
  pthread_mutex_unlock(&control_mutex);
  assert(pthread_create(&query_thread, NULL, query_thread_main, NULL) == 0);
  pthread_mutex_lock(&control_mutex);
  while (!query_attempted) pthread_cond_wait(&control_cond, &control_mutex);
  assert(at_calls == 0 && deinit_calls == 0);
  release_join = true;
  pthread_cond_broadcast(&control_cond);
  pthread_mutex_unlock(&control_mutex);
  void* close_result;
  void* query_result;
  assert(pthread_join(closer, &close_result) == 0);
  assert(pthread_join(query_thread, &query_result) == 0);
  assert((intptr_t)close_result == H2_PAL_ERR_IO);
  assert((intptr_t)query_result == H2_PAL_ERR_BUSY);
  assert(s_ppp_rx_thread != NULL && s_ppp != NULL && at_calls == 0);
}
static void test_reopen_after_peer_loss(void) {
  active();
  modem_ppp_status(&pcb, 1, NULL);
  connected_on_wait = true;
  assert(modem_data_open_adapter(&s_modem, 20) == H2_PAL_OK);
  assert(join_calls == 1 && free_calls == 1);
  assert(s_ppp == &pcb && s_ppp_rx_thread != NULL && raw_owned);
  assert(modem_data_snapshot().state == H2_PAL_MODEM_DATA_OPEN);
}
int main(void) {
  test_peer_reopen();
  test_close_timeout_retry();
  test_link_up_is_not_terminal();
  test_failures_retry();
  test_query_errors_and_ota();
  test_reopen_after_peer_loss();
  test_query_waits_for_close();
  puts("BK modem retained-resource lifecycle tests passed");
  return 0;
}
