#include "h2/pal/core/h2_pal_errors.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Host stand-ins for the native transport and its task-local dependencies. */
typedef bool h2_atomic_bool_t;
enum { H2_ATOMIC_RELEASE, H2_ATOMIC_ACQUIRE };
static bool h2_atomic_bool_load(const h2_atomic_bool_t* value, int order) {
  (void)order;
  return *value;
}
static void h2_atomic_bool_store(h2_atomic_bool_t* value, bool next,
                                 int order) {
  (void)order;
  *value = next;
}
typedef struct {
  int marker;
} h2_runtime_t;
typedef struct {
  int marker;
} h2_loader_t;
typedef struct {
  void* operation_mutex;
} h2_loader_command_config_t;
typedef struct {
  h2_loader_command_config_t config;
} h2_loader_command_t;
typedef int h2_loader_startup_action_t;
typedef struct {
  void* user;
  int (*poll_command_service)(void*, h2_runtime_t*,
                              const h2_loader_command_config_t*);
  int (*stop_command_service)(void*, h2_runtime_t*);
} h2_esp_h2loader_config_t;
typedef struct {
  h2_runtime_t* runtime;
  const h2_esp_h2loader_config_t* config;
} h2_esp_h2loader_context_t;
typedef struct {
  uint32_t pending_conv;
} transport_t;

static h2_atomic_bool_t s_command_stop_requested;
static transport_t s_command_transport;
static unsigned usb_polls, service_polls, service_stops, sleeps, activations;
static bool active_session, locks_held;
static int marker;
static h2_runtime_t runtime;
static int stop_serve_loader(void* user);

static int poll_service(void* user, h2_runtime_t* value,
                        const h2_loader_command_config_t* command) {
  assert(user == &marker && value == &runtime && !locks_held);
  assert(command->operation_mutex == &marker);
  ++service_polls;
  assert(service_polls <= 6u);
  if (service_polls <= 2u) return H2_PAL_ERR_IO;
  if (service_polls == 3u) return H2_PAL_ERR_WOULD_BLOCK;
  return H2_PAL_OK;
}

static int stop_service(void* user, h2_runtime_t* value) {
  assert(user == &marker && value == &runtime && !locks_held);
  assert(s_command_stop_requested);
  ++service_stops;
  assert(service_stops <= 5u);
  if (service_stops <= 2u) return H2_PAL_ERR_BUSY;
  if (service_stops == 3u) return H2_PAL_ERR_WOULD_BLOCK;
  if (service_stops == 4u) return H2_PAL_ERR_IO;
  return H2_PAL_OK;
}

static void sleep_ms(void* user, uint32_t delay_ms) {
  (void)user;
  assert(delay_ms == 10u);
  ++sleeps;
}

static int h2_loader_command_init(h2_loader_command_t* command,
                                  const h2_loader_command_config_t* config) {
  command->config = *config;
  return H2_PAL_OK;
}

static int h2_esp_h2loader_command_transport_activate_pending(
    transport_t* value) {
  assert(value == &s_command_transport && value->pending_conv != 0u);
  ++activations;
  value->pending_conv = 0u;
  return H2_PAL_OK;
}

static bool h2_esp_h2loader_command_transport_has_session(transport_t* value) {
  assert(value == &s_command_transport);
  return active_session;
}

static void poll_usb(void) {
  ++usb_polls;
  assert(usb_polls <= 9u);
  if (usb_polls == 5u) assert(stop_serve_loader(NULL) == H2_PAL_OK);
}

static int h2_esp_h2loader_command_transport_poll_session(transport_t* value,
                                                          uint32_t timeout_ms) {
  assert(value == &s_command_transport && timeout_ms == 50u);
  poll_usb();
  return H2_PAL_ERR_WOULD_BLOCK;
}

static int h2_loader_command_poll(h2_loader_command_t* command,
                                  uint32_t timeout_ms) {
  assert(command->config.operation_mutex == &marker && timeout_ms == 50u);
  locks_held = true;
  poll_usb();
  locks_held = false;
  return H2_PAL_OK;
}

static bool h2_esp_h2loader_command_transport_replacement_pending(
    transport_t* value) {
  (void)value;
  return false;
}
static bool h2_esp_h2loader_command_transport_close_pending(
    transport_t* value) {
  (void)value;
  return false;
}
static void h2_esp_h2loader_command_transport_deactivate(transport_t* value) {
  (void)value;
}

/* FUNCTIONS */

int main(int argc, char** argv) {
  assert(argc == 2);
  h2_esp_h2loader_config_t config = {
      .user = &marker,
      .poll_command_service = poll_service,
      .stop_command_service = stop_service,
  };
  h2_esp_h2loader_context_t context = {.runtime = &runtime, .config = &config};
  h2_loader_command_t command = {.config = {.operation_mutex = &marker}};
  bool early_stop = strcmp(argv[1], "early-stop") == 0;
  bool no_hooks = strcmp(argv[1], "defaults") == 0;
  active_session = strcmp(argv[1], "usb-session") == 0;
  if (strcmp(argv[1], "activate") == 0) s_command_transport.pending_conv = 7u;
  if (no_hooks) config.poll_command_service = NULL;
  if (no_hooks) config.stop_command_service = NULL;
  s_command_stop_requested = early_stop;
  assert(serve_loader(&context, NULL, &command, 0) == H2_PAL_OK);
  if (no_hooks) {
    assert(service_polls == 0u && service_stops == 0u && usb_polls == 5u);
    assert(sleeps == 0u);
  } else {
    assert(service_stops == 5u && sleeps == 4u);
    assert(service_polls == (early_stop ? 0u : 5u + activations));
    assert(usb_polls == (early_stop ? 4u : 9u));
  }
  return 0;
}
