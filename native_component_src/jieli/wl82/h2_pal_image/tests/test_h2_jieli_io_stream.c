#include <assert.h>
#include <string.h>

/* Compile the actual native CDC state machine with only SDK calls faked. */
#define H2_JIELI_CONSOLE_HOST_TEST 1
#include "h2_jieli_wl82_console.c"

static uint32_t now;
static unsigned create_calls, fail_create, live_objects, tasks, handlers;
static unsigned board_calls, transmitted, debug_attempts;
static int board_error, worker_error, block_tx;
static void* worker_user;
static void (*rx_callback)(struct usb_device_t*);

uint32_t timer_get_ms(void) { return now; }
void os_time_dly(unsigned ticks) { now += ticks * 10u; }
int os_mutex_create(OS_MUTEX* mutex) {
  assert(!mutex->live);
  if (++create_calls == fail_create) return -1;
  mutex->live = 1;
  ++live_objects;
  return OS_NO_ERR;
}
int os_mutex_del(OS_MUTEX* mutex, int force) {
  assert(!force && mutex->live && !mutex->held && !tasks && !handlers);
  mutex->live = 0;
  --live_objects;
  return OS_NO_ERR;
}
int os_mutex_accept(OS_MUTEX* mutex) {
  assert(mutex->live);
  if (mutex->held) return -1;
  mutex->held = 1;
  return OS_NO_ERR;
}
int os_mutex_pend(OS_MUTEX* mutex, int ticks) {
  if (mutex->held) {
    assert(ticks > 0);
    now += (uint32_t)ticks * 10u;
    return -1;
  }
  return os_mutex_accept(mutex);
}
int os_mutex_post(OS_MUTEX* mutex) {
  assert(mutex->live && mutex->held);
  mutex->held = 0;
  return OS_NO_ERR;
}
int os_sem_create(OS_SEM* sem, int count) {
  assert(!sem->live && count == 0);
  if (++create_calls == fail_create) return -1;
  sem->live = 1;
  sem->count = count;
  ++live_objects;
  return OS_NO_ERR;
}
int os_sem_del(OS_SEM* sem, int force) {
  assert(sem->live && !force && !tasks && !handlers);
  sem->live = 0;
  --live_objects;
  return OS_NO_ERR;
}
int os_sem_pend(OS_SEM* sem, unsigned ticks) {
  assert(sem->live);
  if (sem->count) {
    --sem->count;
    return OS_NO_ERR;
  }
  assert(ticks); /* Zero in the SDK would block forever. */
  now += ticks * 10u;
  return -1;
}
int os_sem_post(OS_SEM* sem) {
  assert(sem->live);
  ++sem->count;
  return OS_NO_ERR;
}
int os_task_create(void (*entry)(void*), void* user, int priority, int stack,
                   int queue, const char* name) {
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  (void)entry;
  (void)user;
  (void)priority;
  (void)stack;
  (void)queue;
  (void)name;
  assert(0);
  return -1;
#else
  assert(entry == usb_rx_task && user == &state && priority == 12 &&
         stack == 4096 && queue == 0 && !strcmp(name, "h2console_rx"));
  assert(live_objects == 4u && !tasks && !handlers);
  if (worker_error) return -1;
  worker_user = user;
  ++tasks;
  return OS_NO_ERR;
#endif
}
usb_dev usb_device2id(struct usb_device_t* device) { return device->id; }
uint32_t cdc_read_data(usb_dev id, uint8_t* data, uint32_t len) {
  (void)id;
  (void)data;
  (void)len;
  return 0u;
}
uint32_t cdc_write_data(usb_dev id, uint8_t* data, uint32_t len) {
  assert(id == 1u && data != NULL && state.tx_mutex.held);
  /* A log producer cannot splice bytes into this frame's short-write loop. */
  ++debug_attempts;
  assert(h2_jieli_usb_debug_try_lock() == 0);
  if (block_tx) return 0u;
  uint32_t count = len > 2u ? 2u : len;
  transmitted += count;
  return count;
}
void cdc_set_wakeup_handler(usb_dev id, void (*handler)(struct usb_device_t*)) {
  assert(id < USB_MAX_HW_NUM && tasks == 1u && live_objects == 4u);
  rx_callback = handler;
  ++handlers;
}
static int board_start(void) {
  ++board_calls;
  return board_error;
}
static int board_read(void* buffer, size_t len) {
  assert(buffer != NULL && len != 0u);
  *(char*)buffer = 'u';
  return 1;
}
static int board_write(const void* buffer, size_t len, uint32_t timeout_ms) {
  (void)timeout_ms;
  assert(buffer != NULL);
  return len > 2u ? 2 : (int)len;
}
const h2_jieli_wl82_console_config_t* h2_jieli_wl82_console_board_config(void) {
  static const h2_jieli_wl82_console_config_t config = {
      .start = board_start, .read = board_read, .write = board_write};
  return &config;
}

int main(void) {
  const h2_pal_io_stream_api_t* stream = h2_jieli_wl82_platform_io_stream_api();
  assert(create_calls == 0u && board_calls == 0u && live_objects == 0u);
  char before;
  size_t before_count = 99u;
  assert(h2_pal_io_stream_read(stream, &before, 1u, &before_count, 0u) ==
         H2_PAL_ERR_CLOSED);
  assert(before_count == 0u &&
         h2_pal_io_stream_flush(stream) == H2_PAL_ERR_CLOSED);
  for (fail_create = 1u; fail_create <= 4u; ++fail_create) {
    create_calls = 0u;
    assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_NO_MEMORY);
    assert(live_objects == 0u && !tasks && !handlers && !state.initialized);
  }
  fail_create = 0u;
  board_error = H2_PAL_ERR_IO;
  assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_IO);
  assert(live_objects == 0u && !tasks && !state.initialized);
  board_error = H2_PAL_OK;
#if !defined CONFIG_H2_UART1_DEBUG_ENABLE
  worker_error = 1;
  assert(h2_pal_io_stream_start(stream) == H2_PAL_ERR_NO_MEMORY);
  assert(live_objects == 0u && !tasks && !handlers && !state.initialized);
  worker_error = 0;
#endif
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK);
#if defined CONFIG_H2_UART1_DEBUG_ENABLE
  (void)worker_error;
  (void)worker_user;
  assert(live_objects == 4u && !tasks && !handlers);
  unsigned saved_creates = create_calls, saved_board_calls = board_calls;
  char bytes[8];
  size_t count;
  assert(h2_pal_io_stream_read(stream, bytes, sizeof(bytes), &count, 0u) ==
             H2_PAL_OK &&
         count == 1u && bytes[0] == 'u');
  assert(h2_pal_io_stream_write(stream, "frame!", 6u, &count, 20u) ==
             H2_PAL_ERR_TIMEOUT &&
         count == 2u);
  assert(h2_pal_io_stream_write(stream, "frame!", 6u, &count, 0u) ==
             H2_PAL_ERR_WOULD_BLOCK &&
         count == 2u);
  assert(h2_pal_io_stream_write(stream, "f", 1u, &count, 0u) == H2_PAL_OK &&
         count == 1u);
  assert(h2_pal_io_stream_flush(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK);
  assert(live_objects == 4u && !tasks && !handlers);
  assert(create_calls == saved_creates && board_calls == saved_board_calls);
#else
  assert(live_objects == 4u && tasks == 1u && handlers == USB_MAX_HW_NUM);
  unsigned saved_creates = create_calls, saved_board_calls = board_calls;
  assert(worker_user == &state && rx_callback != NULL);
  struct usb_device_t device = {.id = 1u};
  rx_callback(&device);
  assert(state.usb_id == 1u && state.rx_wakeup_sem.count == 1);
  state.rx_ring[0] = 'r';
  state.rx_count = state.rx_head = 1u;
  char bytes[8];
  size_t count;
  assert(h2_pal_io_stream_read(stream, bytes, sizeof(bytes), &count, 0u) ==
         H2_PAL_OK);
  assert(count == 1u && bytes[0] == 'r');
  assert(h2_pal_io_stream_write(stream, "frame!", 6u, &count, 20u) ==
         H2_PAL_OK);
  assert(count == 6u && transmitted == 6u && debug_attempts == 3u);
  assert(!state.tx_mutex.held);
  assert(h2_jieli_usb_debug_try_lock() == 1);
  h2_jieli_usb_debug_unlock();
  block_tx = 1;
  assert(h2_pal_io_stream_write(stream, "frame!", 6u, &count, 20u) ==
         H2_PAL_ERR_TIMEOUT);
  assert(count == 0u && !state.tx_mutex.held && now == 20u);
  assert(h2_pal_io_stream_flush(stream) == H2_PAL_OK);
  state.rx_ring[1] = 's';
  state.rx_head = 2u;
  state.rx_count = 1u;
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_release(stream) == H2_PAL_OK);
  assert(h2_pal_io_stream_start(stream) == H2_PAL_OK);
  assert(create_calls == saved_creates && board_calls == saved_board_calls);
  assert(live_objects == 4u && tasks == 1u && handlers == USB_MAX_HW_NUM);
  assert(h2_pal_io_stream_read(stream, bytes, sizeof(bytes), &count, 0u) ==
         H2_PAL_OK);
  assert(count == 1u && bytes[0] == 's');
#endif
  return 0;
}
