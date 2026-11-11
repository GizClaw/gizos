#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "h2_h2loader_host_internal.h"
#include "h2_iostreamikcp.h"

#define H2_H2LOADER_HOST_SERIAL_POLL_MS 10u
#define H2_H2LOADER_HOST_SERIAL_RESPONSE_ACK_GRACE_MS 100u

/* Production serial response parsing, ACK-grace handling and monitoring are
 * inserted below. Only the framed stream and clock are scripted. */
struct h2_iostreamikcp {
  uint8_t queue[2048];
  size_t length;
  size_t offset;
  unsigned polls;
  int fast_boot;
  void* log_user;
};

static uint64_t clock_ms;
static unsigned accept_count;
static const char accepted[] =
    "H2_LOADER_REBOOT target=loader result=accepted\n";
static const char final[] =
    "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\r\n";
static const char boot[] = "ESP boot\nH2_LOADER_READY status=ready\n";

static h2_pal_result_t serial_stream_log(void*, const uint8_t*, size_t);

int h2_h2loader_host_is_sha256(const char* text) {
  (void)text;
  assert(0 && "reboot does not validate a staged package checksum");
  return 0;
}

h2_pal_result_t h2_iostreamikcp_read(h2_iostreamikcp_t* stream, uint8_t* out,
                                     size_t size, size_t* count) {
  *count = stream->length - stream->offset;
  if (*count == 0u) return H2_PAL_ERR_WOULD_BLOCK;
  if (*count > size) *count = size;
  memcpy(out, stream->queue + stream->offset, *count);
  stream->offset += *count;
  return H2_PAL_OK;
}

static void enqueue(h2_iostreamikcp_t* stream, const char* text) {
  assert(stream->length + strlen(text) <= sizeof(stream->queue));
  memcpy(stream->queue + stream->length, text, strlen(text));
  stream->length += strlen(text);
}

h2_pal_result_t h2_iostreamikcp_poll(h2_iostreamikcp_t* stream,
                                     uint32_t timeout_ms) {
  clock_ms += timeout_ms;
  ++stream->polls;
  if (stream->polls == 1u) {
    enqueue(stream, accepted);
  } else if (stream->polls == 2u) {
    /* The accepted terminal has been parsed. FINAL arrives during ACK grace,
     * after the response reader returned, and remains queued for monitoring. */
    assert(accept_count == 1u);
    enqueue(stream, final);
    if (stream->fast_boot) {
      for (size_t i = 0u; i < sizeof(boot) - 1u; ++i) {
        h2_pal_result_t rc =
            serial_stream_log(stream->log_user, (const uint8_t*)boot + i, 1u);
        if (rc != H2_PAL_OK) return rc;
      }
    }
  }
  return H2_PAL_ERR_TIMEOUT;
}

h2_pal_result_t h2_iostreamikcp_update(h2_iostreamikcp_t* stream,
                                       uint32_t now_ms) {
  (void)stream;
  (void)now_ms;
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_flush(h2_iostreamikcp_t* stream) {
  (void)stream;
  return H2_PAL_OK;
}

/* SERIAL_DEFINITIONS */
/* SERIAL_NOW */
/* SERIAL_PUMP */
/* SERIAL_LOG */
/* SERIAL_READ */

static h2_pal_result_t serial_command_write(void* transport, const char* line) {
  (void)transport;
  assert(strcmp(line, "h2loader reboot loader\n") == 0);
  return H2_PAL_OK;
}

/* SERIAL_COMMAND */
/* SERIAL_MONITOR */

typedef struct {
  int (*on_log)(void*, const uint8_t*, size_t);
  void* log_user;
} test_config_t;
typedef struct {
  size_t output_bytes;
  size_t log_bytes;
} test_result_t;
typedef struct {
  test_config_t* config;
  test_result_t* case_result;
  size_t monitor_output_bytes;
  uint8_t monitor_logs;
  uint8_t monitor_reboot;
  size_t monitor_prefix_len;
  uint8_t monitor_command_line;
  uint8_t monitor_console_line;
} h2_e2e_transport_context_t;

/* RUNNER_LOG */

static void accepted_callback(void* user) {
  ++accept_count;
  reboot_monitor_accepted(user);
}

static h2_pal_result_t monotonic(void* user, uint64_t* now) {
  (void)user;
  *now = clock_ms;
  return H2_PAL_OK;
}

static int bounded(void* user) {
  unsigned* remaining = user;
  if (*remaining == 0u) return 1;
  --*remaining;
  return 0;
}

int main(void) {
  (void)serial_status_diagnostics;
  (void)serial_stream_now_ms;
  (void)find_line_marker;
  for (int fast_boot = 0; fast_boot < 2; ++fast_boot) {
    clock_ms = 0u;
    accept_count = 0u;
    h2_iostreamikcp_t stream = {.fast_boot = fast_boot};
    const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = monotonic};
    const h2_pal_time_api_t time = {.vtable = &time_vtable};
    test_config_t config = {0};
    test_result_t evidence = {0};
    h2_e2e_transport_context_t context = {
        .config = &config,
        .case_result = &evidence,
        .monitor_reboot = 1u,
    };
    h2_h2loader_host_serial_connection_t connection = {
        .time = &time,
        .stream = &stream,
        .on_log = monitor_output,
        .log_user = &context,
        .command_timeout_ms = 1000u,
    };
    stream.log_user = &connection;
    h2_h2loader_host_status_t status = {
        .active_role = H2_H2LOADER_HOST_ACTIVE_ROLE_LOADER,
        .command_availability =
            H2_H2LOADER_HOST_COMMAND_AVAILABLE_REBOOT_LOADER,
    };
    h2_h2loader_host_command_request_t request = {
        .command = H2_H2LOADER_HOST_COMMAND_REBOOT_LOADER,
        .status = &status,
        .on_accepted = accepted_callback,
        .accepted_user = &context,
    };
    h2_h2loader_host_command_result_t result = {0};
    assert(h2_h2loader_host_serial_execute_command(&connection, &request,
                                                   &result) == H2_PAL_OK);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
    assert(accept_count == 1u && context.monitor_logs);
    assert(context.monitor_output_bytes ==
           (fast_boot ? sizeof(boot) - 1u : 0u));
    unsigned polls = 4u;
    assert(h2_h2loader_host_serial_monitor_logs(&connection, bounded, &polls) ==
           H2_PAL_EXIT);
    /* The real serial monitor drained FINAL, but it contributed no evidence.
     * A boot entirely inside the ACK grace period remains accounted for. */
    assert(stream.offset == stream.length);
    assert(context.monitor_output_bytes ==
           (fast_boot ? sizeof(boot) - 1u : 0u));
  }
  return 0;
}
