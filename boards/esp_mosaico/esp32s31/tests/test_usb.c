#ifdef NDEBUG
#undef NDEBUG
#endif
#include "h2_mosaico_usb_test_sdk.h"
#include "h2_mosaico_usb_transport.h"
#include "h2_mosaico_usb_console.h"
#include <assert.h>
#include <string.h>
#include <fcntl.h>
static int64_t now;
static bool endpoint_ready = true, blocked, nested;
static int drivers, cdcs, clears, rx_clears, queued, delay_calls;
static esp_vfs_t console;
static void (*line_callback)(int, cdcacm_event_t*);
static int block_after;
static bool disconnect_during_clear, flush_blocked;
static SemaphoreHandle_t mutex;
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* s) {
  mutex = s;
  return s;
}
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
  (void)t;
  if (s->held) return 0;
  s->held = 1;
  return 1;
}
void xSemaphoreGive(SemaphoreHandle_t s) {
  assert(s->held);
  s->held = 0;
}
void vTaskDelay(TickType_t n) {
  now += n * 1000;
  delay_calls++;
}
int64_t esp_timer_get_time(void) { return now; }
int tinyusb_driver_install(const tinyusb_config_t* c) {
  (void)c;
  drivers++;
  return ESP_OK;
}
int tinyusb_cdcacm_init(const tinyusb_config_cdcacm_t* c) {
  assert(c->cdc_port == 0);
  line_callback = c->callback_line_state_changed;
  cdcs++;
  return ESP_OK;
}
bool tud_cdc_n_ready(int n) {
  assert(n == 0);
  return endpoint_ready;
}
bool tud_cdc_n_write_clear(int n) {
  assert(n == 0);
  clears++;
  if (disconnect_during_clear) {
    disconnect_during_clear = false;
    cdcacm_event_t event = {.line_state_changed_data = {.dtr = true}};
    line_callback(0, &event);
    event.line_state_changed_data.dtr = false;
    line_callback(0, &event);
  }
  return true;
}
void tud_cdc_n_read_flush(int n) {
  assert(n == 0 && mutex->held);
  rx_clears++;
}
size_t tinyusb_cdcacm_write_queue(int n, const void* p, size_t size) {
  assert(n == 0 && p && mutex->held);
  if (nested) {
    nested = false;
    /* Exercise the actual registered VFS path while a frame owns TX. */
    assert(console.write(0, "log", 3) == -1);
  }
  if (blocked || (block_after && queued >= block_after)) return 0;
  size_t count = size > 2 ? 2 : size;
  queued += (int)count;
  return count;
}
int tinyusb_cdcacm_write_flush(int n, TickType_t t) {
  assert(t == 0); /* Blocking SDK flush may log recursively on timeout. */
  assert(n == 0 && mutex->held);
  return flush_blocked ? ESP_ERR_NOT_FINISHED : ESP_OK;
}
int tinyusb_cdcacm_read(int n, void* p, size_t size, size_t* out) {
  assert(n == 0);
  if (size) {
    *(char*)p = 'x';
    *out = 1;
  }
  return ESP_OK;
}
int esp_vfs_register(const char* path, const esp_vfs_t* v, void* user) {
  (void)user;
  assert(!strcmp(path, "/dev/h2cdc"));
  console = *v;
  return ESP_OK;
}
FILE* h2_test_freopen(const char* p, const char* m, FILE* f) {
  assert(!strcmp(p, "/dev/h2cdc/0") && !strcmp(m, "w"));
  assert(console.open && console.close);
  assert(console.open("/0", O_WRONLY | O_CREAT | O_TRUNC, 0666) == 0);
  assert(console.open("/other", O_WRONLY, 0) == -1);
  assert(console.open("/0", O_RDONLY, 0) == -1);
  assert(console.close(0) == 0);
  return f;
}
int main(void) {
  assert(h2_mosaico_usb_console_init() == H2_PAL_OK);
  assert(h2_mosaico_usb_console_init() == H2_PAL_OK && drivers == 1 &&
         cdcs == 1);
  /* The first opener may keep DTR low. Discard any boot-time queued bytes
   * once, then permit its handshake without requiring a high transition. */
  cdcacm_event_t initial_low = {.line_state_changed_data = {.dtr = false}};
  line_callback(0, &initial_low);
  size_t initial_out = 0;
  char initial_data;
  assert(h2_mosaico_usb_read(NULL, &initial_data, 1, &initial_out, 0) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_mosaico_usb_read(NULL, &initial_data, 1, &initial_out, 0) ==
         H2_PAL_OK);
  assert(initial_out == 1 && clears == 1 && rx_clears == 1);
  clears = rx_clears = 0;
  cdcacm_event_t connected_event = {.line_state_changed_data = {.dtr = true}};
  line_callback(0, &connected_event);
  size_t out = 99;
  char data[8];
  nested = true;
  assert(h2_mosaico_usb_write(NULL, "frame!", 6, &out, 20) == H2_PAL_OK &&
         out == 6 && queued == 6);
  assert(!mutex->held);
  assert(console.write(0, "log", 3) == 3);
  block_after = queued + 2;
  assert(h2_mosaico_usb_write(NULL, "frame", 5, &out, 2) == H2_PAL_ERR_CLOSED &&
         out == 2);
  assert(!mutex->held && clears == 1 && delay_calls > 0);
  assert(console.write(0, "log", 3) == -1);
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  cdcacm_event_t event = {.line_state_changed_data = {.dtr = false}};
  assert(line_callback);
  line_callback(0, &event);
  /* A short DTR drop must be retained even if polling never sees
   * !endpoint_ready. */
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  /* Host Core holds DTR low during its handshake. Endpoints remain ready:
   * recover the new byte stream without waiting for DTR to be asserted. */
  block_after = 0;
  blocked = false;
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) == H2_PAL_OK &&
         out == 1);
  assert(console.write(0, "log", 3) == 3 && !mutex->held && rx_clears == 1);
  /* TinyUSB reports every SET_CONTROL_LINE_STATE, including an RTS-only or
   * redundant DTR-low request. It must not close the active new stream. */
  line_callback(0, &event);
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) == H2_PAL_OK);
  assert(rx_clears == 1 && console.write(0, "log", 3) == 3);
  assert(h2_mosaico_usb_write(NULL, NULL, 1, &out, 0) ==
         H2_PAL_ERR_INVALID_ARG);
  line_callback(0, &connected_event);
  event.line_state_changed_data.dtr = false;
  line_callback(0, &event);
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  disconnect_during_clear = true;
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  assert(console.write(0, "log", 3) == -1);
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) == H2_PAL_OK);

  endpoint_ready = false;
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) ==
         H2_PAL_ERR_CLOSED);
  assert(h2_mosaico_usb_write(NULL, "frame", 5, &out, 2) == H2_PAL_ERR_CLOSED &&
         out == 0);
  endpoint_ready = true;
  assert(h2_mosaico_usb_read(NULL, data, sizeof(data), &out, 0) == H2_PAL_OK);
  assert(console.write(0, "log", 3) == 3);

  flush_blocked = true;
  assert(h2_mosaico_usb_write(NULL, "frame", 5, &out, 2) == H2_PAL_ERR_CLOSED &&
         out == 5);
  assert(!mutex->held && console.write(0, "log", 3) == -1);
  return 0;
}
