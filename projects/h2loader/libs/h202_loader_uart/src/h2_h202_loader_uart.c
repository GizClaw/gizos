#include "h2_h202_loader_uart.h"

#include "h2_device_serial.h"
#include "h2_loader_task_names.h"

static h2_device_serial_t s_transport;
static h2_loader_command_config_t s_config;
static h2_pal_task_t* s_task;

static void serve_uart(void* user) {
  h2_loader_command_t* command = user;
  for (;;) {
    if (h2_device_serial_close_pending(&s_transport) &&
        s_transport.pending_conv == 0u) {
      h2_device_serial_deactivate(&s_transport);
    }
    if (s_transport.pending_conv != 0u) {
      int rc = h2_loader_command_init(command, &s_config);
      if (rc == H2_PAL_OK) rc = h2_device_serial_activate_pending(&s_transport);
      if (rc != H2_PAL_OK) h2_device_serial_deactivate(&s_transport);
    } else if (h2_device_serial_has_session(&s_transport)) {
      (void)h2_loader_command_poll(command, 50u);
    } else {
      (void)h2_device_serial_poll_session(&s_transport, 50u);
    }
  }
}

int h2_h202_loader_uart_start(h2_runtime_t* runtime,
                              h2_loader_command_t* command,
                              const h2_loader_command_config_t* config) {
  if (runtime == NULL || command == NULL || config == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (s_task != NULL) return H2_PAL_ERR_INVALID_STATE;
  int rc = h2_pal_io_stream_start(runtime->io_stream);
  if (rc != H2_PAL_OK) return rc;
  const h2_device_serial_config_t serial = {
      .physical_io = h2_iostreamikcp_io_from_stream(runtime->io_stream),
      .allocator = runtime->mem,
      .time = runtime->time,
      .post_write_poll_ms = 1u,
  };
  rc = h2_device_serial_init_with_config(&s_transport, &serial);
  if (rc == H2_PAL_OK) {
    s_config = *config;
    s_config.io = h2_device_serial_io(&s_transport);
    s_config.transport = H2_LOADER_COMMAND_TRANSPORT_SERIAL;
    const h2_pal_task_options_t options = {
        .name = h2_loader_uart_command_task_name,
        .min_stack_size = 49152u,
    };
    rc = h2_pal_task_start(runtime->task, &options, serve_uart, command,
                           &s_task);
    if (rc != H2_PAL_OK) h2_device_serial_deinit(&s_transport);
  }
  if (rc != H2_PAL_OK) {
    const int release_rc = h2_pal_io_stream_release(runtime->io_stream);
    if (release_rc != H2_PAL_OK) return release_rc;
  }
  return rc;
}
