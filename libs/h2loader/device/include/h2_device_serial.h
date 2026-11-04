#ifndef H2_DEVICE_SERIAL_H
#define H2_DEVICE_SERIAL_H

#include "h2_atomic.h"
#include "h2_command.h"
#include "h2_iostreamikcp.h"
#include "h2_loader_app_client.h"
#include "h2/pal/os/h2_pal_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Zero-initialize once per boot. Descriptor copies retain borrowed userdata.
 */
typedef struct h2_device_serial_endpoint {
  h2_iostreamikcp_io_t io;
  int configured;
  int initialization_started;
} h2_device_serial_endpoint_t;

h2_pal_result_t h2_device_serial_endpoint_configure(
    h2_device_serial_endpoint_t* endpoint, const h2_iostreamikcp_io_t* io);
/** First initialization locks injection, even if native startup fails. Explicit
 * physical injection skips native setup. A failed native setup can retry. */
h2_pal_result_t h2_device_serial_endpoint_start(
    h2_device_serial_endpoint_t* endpoint,
    const h2_iostreamikcp_io_t* default_io,
    h2_pal_result_t (*initialize)(void* user), void* user);

typedef struct h2_device_serial_config {
  h2_iostreamikcp_io_t physical_io;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_time_api_t* time;
  const h2_atomic_bool_t* stop_requested;
  h2_atomic_bool_t* sessions_admitted;
  uint32_t admission_fallback_ms;
  uint32_t idle_timeout_ms;
  uint32_t post_write_poll_ms;
  /** Zero selects the common window of 20 segments. */
  uint32_t receive_window;
} h2_device_serial_config_t;

/** One command task owns the parser/KCP state. Physical I/O and all public PAL
 * objects remain borrowed; the engine never owns a native driver or RX task. */
typedef struct h2_device_serial {
  h2_device_serial_config_t config;
  h2_iostreamikcp_io_t physical_io;
  h2_iostreamikcp_filter_t filter;
  h2_iostreamikcp_t* stream;
  const h2_pal_mem_api_t* allocator;
  const h2_pal_time_api_t* time;
  uint32_t conv, pending_conv, write_timeout_ms, receive_window;
  uint32_t io_started_ms, io_timeout_ms;
  uint32_t last_frame_ms, gate_started_ms;
  int io_deadline_active;
  int replacement_pending, close_pending;
} h2_device_serial_t;

h2_pal_result_t h2_device_serial_init_with_config(
    h2_device_serial_t* transport, const h2_device_serial_config_t* config);
h2_pal_result_t h2_device_serial_init(h2_device_serial_t* transport,
                                      const h2_pal_mem_api_t* allocator,
                                      const h2_pal_time_api_t* time,
                                      const h2_iostreamikcp_io_t* physical_io);
/** Close only the reliable session. The shared physical driver stays installed.
 */
void h2_device_serial_deinit(h2_device_serial_t* transport);
h2_command_io_api_t h2_device_serial_io(h2_device_serial_t* transport);
h2_pal_result_t h2_device_serial_poll_session(h2_device_serial_t* transport,
                                              uint32_t timeout_ms);
h2_pal_result_t h2_device_serial_activate_pending(
    h2_device_serial_t* transport);
uint32_t h2_device_serial_now(h2_device_serial_t* transport);
int h2_device_serial_has_session(const h2_device_serial_t* transport);
int h2_device_serial_replacement_pending(const h2_device_serial_t* transport);
int h2_device_serial_close_pending(const h2_device_serial_t* transport);
void h2_device_serial_deactivate(h2_device_serial_t* transport);

/** Caller-owned startup state survives command-task creation failures. A retry
 * may not replace the client, allocator, or physical descriptor. */
typedef struct h2_device_app_serial {
  h2_device_serial_t transport;
  h2_command_io_api_t io;
  h2_loader_app_client_t* client;
  const h2_pal_task_api_t* task;
  h2_atomic_bool_t stop_requested;
  h2_atomic_bool_t sessions_admitted;
  int admission_requested;
  int initialized;
  int started;
} h2_device_app_serial_t;

typedef struct h2_device_app_serial_config {
  h2_device_serial_config_t transport;
  const h2_pal_task_api_t* task;
  const char* task_name;
  size_t stack_size;
} h2_device_app_serial_config_t;

int h2_device_app_serial_start_with_config(
    h2_device_app_serial_t* state, h2_loader_app_client_t* client,
    const h2_device_app_serial_config_t* config);
int h2_device_app_serial_start(h2_device_app_serial_t* state,
                               h2_loader_app_client_t* client,
                               const h2_pal_task_api_t* task,
                               const h2_pal_mem_api_t* allocator,
                               const h2_pal_time_api_t* time,
                               const h2_iostreamikcp_io_t* physical_io,
                               size_t stack_size);
int h2_device_app_serial_stop(h2_device_app_serial_t* state);
void h2_device_app_serial_admit_sessions(h2_device_app_serial_t* state);

#ifdef __cplusplus
}
#endif
#endif
