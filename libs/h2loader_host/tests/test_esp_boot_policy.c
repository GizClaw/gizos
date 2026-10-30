#include "h2_h2loader_host_flash.h"
#include "h2_desktop_platform.h"
#include "esp_loader_io.h"

#include <assert.h>
#include <string.h>

typedef struct transition {
  uint32_t mask;
  uint32_t value;
} transition_t;
typedef struct fixture {
  transition_t transitions[32];
  size_t count;
} fixture_t;

static h2_pal_result_t open_port(void* user, const char* name,
                                 const h2_pal_uart_io_stream_config_t* config,
                                 h2_pal_serial_host_session_t** out) {
  (void)name;
  (void)config;
  *out = (h2_pal_serial_host_session_t*)user;
  return 0;
}
static h2_pal_result_t stream(void* user, h2_pal_serial_host_session_t* session,
                              const h2_pal_uart_io_stream_api_t** out) {
  static const h2_pal_uart_io_stream_vtable_t vtable = {0};
  static const h2_pal_uart_io_stream_api_t api = {.vtable = &vtable};
  assert((void*)session == user);
  *out = &api;
  return 0;
}
static h2_pal_result_t control(void* user,
                               h2_pal_serial_host_session_t* session,
                               uint32_t mask, uint32_t value) {
  fixture_t* f = user;
  assert((void*)session == user && f->count < 32);
  f->transitions[f->count++] = (transition_t){mask, value};
  return 0;
}
static h2_pal_result_t delay(void* user, uint32_t ms) {
  fixture_t* f = user;
  assert(f->count < 32);
  f->transitions[f->count++] = (transition_t){0, ms};
  return 0;
}
static h2_pal_result_t close_port(void* user,
                                  h2_pal_serial_host_session_t** session) {
  assert((void*)*session == user);
  *session = NULL;
  return 0;
}

static void check(h2_h2loader_host_esp_boot_policy_t policy,
                  const transition_t* expected, size_t count) {
  fixture_t f = {0};
  const h2_pal_serial_host_vtable_t serial_vtable = {
      .open = open_port,
      .session_stream = stream,
      .set_control_lines = control,
      .close = close_port};
  const h2_pal_serial_host_api_t serial = {.user = &f,
                                           .vtable = &serial_vtable};
  const h2_pal_time_vtable_t time_vtable = {.sleep_ms = delay};
  const h2_pal_time_api_t time = {.user = &f, .vtable = &time_vtable};
  const h2_h2loader_host_esp_flash_config_t config = {
      .serial = &serial,
      .time = &time,
      .allocator = h2_desktop_platform_default_allocator(),
      .port_id = "test-port",
      .expected_target = "esp32s3",
      .boot_policy = policy};
  h2_h2loader_host_flash_driver_t driver;
  assert(h2_h2loader_host_esp_flash_open(&config, &driver) == 0);
  loader_port_enter_bootloader();
  loader_port_reset_target();
  assert(f.count == count);
  if (count != 0)
    assert(memcmp(f.transitions, expected, count * sizeof(*expected)) == 0);
  assert(driver.vtable->close(driver.user) == 0);
}

int main(void) {
  enum {
    D = H2_PAL_SERIAL_HOST_CONTROL_DTR,
    R = H2_PAL_SERIAL_HOST_CONTROL_RTS
  };
  const transition_t classic[] = {{D | R, R}, {0, 50}, {D | R, D}, {0, 100},
                                  {D | R, 0}, {0, 50}, {D | R, R}, {0, 100},
                                  {D | R, 0}, {0, 50}};
  const transition_t usb[] = {
      {R, 0}, {D, 0},   {0, 100}, {D, D}, {R, 0}, {0, 100}, {R, R}, {D, 0},
      {R, R}, {0, 100}, {D, 0},   {R, 0}, {R, R}, {0, 200}, {R, 0}, {0, 200}};
  check(H2_H2LOADER_HOST_ESP_BOOT_MANUAL, NULL, 0);
  check(H2_H2LOADER_HOST_ESP_BOOT_DTR_RTS, classic,
        sizeof(classic) / sizeof(classic[0]));
  check(H2_H2LOADER_HOST_ESP_BOOT_USB_SERIAL_JTAG, usb,
        sizeof(usb) / sizeof(usb[0]));
  return 0;
}
