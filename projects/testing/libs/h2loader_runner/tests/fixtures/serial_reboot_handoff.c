#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "h2_h2loader_host_internal.h"
#include "h2_iostreamikcp.h"

#define H2_H2LOADER_HOST_SERIAL_POLL_MS 10u
#define H2_H2LOADER_HOST_SERIAL_RESPONSE_ACK_GRACE_MS 100u
#define H2_H2LOADER_HOST_SERIAL_RESPONSE_SIZE 8192u
#define H2_H2LOADER_HOST_SERIAL_WRITE_BATCH (64u * 1024u)
#define H2_H2LOADER_HOST_SERIAL_SESSION_CLOSE_TIMEOUT_MS 1000u

/* FRAME */

/* Production serial response parsing, ACK-grace handling and monitoring are
 * inserted below. The real frame filter is used for buffered READY replay. */
struct h2_iostreamikcp {
  uint8_t queue[2048];
  size_t length;
  size_t offset;
  unsigned polls;
  int fast_boot;
  int update_closed_once;
  int flush_closed_once;
  int close_phase;
  int final_sent;
  int late_sent;
  int stream_closed;
  int command_written;
  unsigned writes;
  int raw_banners_sent;
  h2_iostreamikcp_filter_t filter;
  void* log_user;
};

static uint64_t clock_ms;
static unsigned accept_count;
static const char accepted[] =
    "H2_LOADER_REBOOT target=loader result=accepted\r\n";
static const char final[] =
    "H2_LOADER_REBOOT_FINAL target=loader result=OK code=0\r\n";
static const char boot[] = "H2_LOADER_READY status=ready\n";

static h2_pal_result_t serial_stream_log(void*, const uint8_t*, size_t);

int h2_h2loader_host_is_sha256(const char* text) {
  if (text == NULL || strlen(text) != 64u) return 0;
  for (size_t i = 0u; i < 64u; ++i)
    if (!((text[i] >= '0' && text[i] <= '9') ||
          (text[i] >= 'a' && text[i] <= 'f')))
      return 0;
  return 1;
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
  if (stream->fast_boot >= 14 && stream->fast_boot <= 18) {
    if (stream->polls == 2u && stream->fast_boot != 15) {
      enqueue(stream,
              "H2_LOADER_STAGE_RECEIVE result=OK\nH2_LOADER_STAGE result=OK\n");
      if (stream->fast_boot == 14 || stream->fast_boot == 16) {
        static const uint8_t diagnostic[] = "stage diagnostic\n";
        return serial_stream_log(stream->log_user, diagnostic,
                                 sizeof(diagnostic) - 1u);
      }
    }
    return H2_PAL_ERR_TIMEOUT;
  }
  if (stream->close_phase && !stream->late_sent &&
      (stream->fast_boot == 6 || stream->fast_boot == 7 ||
       stream->fast_boot == 10 || stream->fast_boot == 11)) {
    stream->late_sent = 1;
    enqueue(stream, boot); /* Reliable DATA console output, not raw UART. */
  }
  if ((stream->polls == 1u && stream->fast_boot != 5 &&
       stream->fast_boot != 20) ||
      (stream->polls == 2u &&
       (stream->fast_boot == 5 || stream->fast_boot == 20))) {
    enqueue(stream, accepted);
    if (stream->fast_boot == 20) {
      static const uint8_t diagnostic[] = "accepted reboot diagnostic\n";
      return serial_stream_log(stream->log_user, diagnostic,
                               sizeof(diagnostic) - 1u);
    }
    if (stream->fast_boot == 12) {
      enqueue(stream, boot);
      return h2_iostreamikcp_input(stream, (const uint8_t*)boot,
                                   sizeof(boot) - 1u);
    }
    if (stream->fast_boot == 2 || stream->fast_boot == 5) {
      /* Acceptance is already decoded when raw boot output follows in this
       * same poll. READY closes it before the ordinary response reader runs. */
      for (size_t i = 0u; i < sizeof(boot) - 1u; ++i) {
        h2_pal_result_t rc =
            serial_stream_log(stream->log_user, (const uint8_t*)boot + i, 1u);
        if (rc != H2_PAL_OK) return rc;
      }
    }
  } else if (accept_count == 1u && !stream->final_sent &&
             stream->filter.len == 0u) {
    stream->final_sent = 1;
    /* The accepted terminal has been parsed. FINAL arrives during ACK grace,
     * after the response reader returned, and remains queued for monitoring. */
    assert(accept_count == 1u);
    enqueue(stream, final);
    if ((stream->fast_boot == 9 || stream->fast_boot == 13 ||
         stream->fast_boot == 19) &&
        !stream->raw_banners_sent) {
      uint8_t both[H2_IOSTREAMIKCP_FILTER_BUFFER_SIZE];
      size_t length = sizeof(boot) - 1u;
      memcpy(both, boot, length);
      if (stream->fast_boot == 9) {
        memcpy(both + length, boot, length);
        length *= 2u;
      } else {
        const h2_iostreamikcp_frame_t frame = {
            .flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
            .conv = 7u,
            .payload = (const uint8_t*)boot,
            .payload_len = sizeof(boot) - 1u,
        };
        size_t encoded = 0u;
        assert(h2_iostreamikcp_frame_encode(&frame, both + length,
                                            sizeof(both) - length,
                                            &encoded) == H2_PAL_OK);
        length += encoded;
      }
      stream->raw_banners_sent = 1;
      return h2_iostreamikcp_input(stream, both, length);
    }
    if (stream->fast_boot == 1 || stream->fast_boot == 7) {
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
  (void)now_ms;
  if (stream->fast_boot == 3 && stream->polls >= 2u &&
      !stream->update_closed_once) {
    stream->update_closed_once = 1;
    return H2_PAL_ERR_CLOSED;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_flush(h2_iostreamikcp_t* stream) {
  if ((stream->fast_boot == 4 || stream->fast_boot == 20 ||
       (stream->fast_boot == 19 && stream->close_phase)) &&
      !stream->flush_closed_once) {
    stream->flush_closed_once = 1;
    return H2_PAL_ERR_CLOSED;
  }
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_write(h2_iostreamikcp_t* stream,
                                      const uint8_t* data, size_t len) {
  if (stream->fast_boot >= 14 && stream->fast_boot <= 18) {
    assert((len > 15u && memcmp(data, "h2loader stage ", 15u) == 0) ||
           (len == 1u && data[0] == 0x42u));
  } else {
    assert(len == strlen("h2loader reboot loader\n"));
    assert(memcmp(data, "h2loader reboot loader\n", len) == 0);
  }
  stream->command_written = 1;
  ++stream->writes;
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_get_stats(h2_iostreamikcp_t* stream,
                                          h2_iostreamikcp_stats_t* stats) {
  memset(stats, 0, sizeof(*stats));
  stats->waitsnd = stream->command_written && stream->polls == 0u ? 1u : 0u;
  if (stream->fast_boot == 13 && stream->close_phase) stats->waitsnd = 1u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_input_frame(
    h2_iostreamikcp_t* stream, const h2_iostreamikcp_frame_t* frame) {
  assert(stream->length + frame->payload_len <= sizeof(stream->queue));
  memcpy(stream->queue + stream->length, frame->payload, frame->payload_len);
  stream->length += frame->payload_len;
  return H2_PAL_OK;
}

static h2_pal_result_t fixture_frame(void* user,
                                     const h2_iostreamikcp_frame_t* frame) {
  return h2_iostreamikcp_input_frame(user, frame);
}

h2_pal_result_t h2_iostreamikcp_input(h2_iostreamikcp_t* stream,
                                      const uint8_t* data, size_t len) {
  return h2_iostreamikcp_filter_input_with_log(
      &stream->filter, data, len, fixture_frame, stream, serial_stream_log,
      stream->log_user);
}

void h2_iostreamikcp_close(h2_iostreamikcp_t* stream) {
  stream->stream_closed = 1;
}

/* SERIAL_DEFINITIONS */
/* SERIAL_NOW */
/* SERIAL_PUMP */
/* SERIAL_DELIVERY */
/* SERIAL_LOG */
/* SERIAL_READ */
/* SERIAL_WRITE */
/* SERIAL_COMMAND */
/* SERIAL_MONITOR */
/* SERIAL_SESSION_CONTEXT */
/* SERIAL_SESSION_FRAME */

static unsigned freed;
static void fixture_free(void* user, void* ptr) {
  (void)user;
  (void)ptr;
  ++freed;
}
static h2_pal_result_t fixture_close(void* user,
                                     h2_pal_serial_host_session_t** session) {
  (void)user;
  *session = NULL;
  return H2_PAL_OK;
}
static const h2_pal_mem_vtable_t memory_vtable = {.free = fixture_free};
static const h2_pal_serial_host_vtable_t serial_vtable = {.close =
                                                              fixture_close};
static h2_pal_result_t serial_session_control(
    h2_h2loader_host_serial_connection_t* connection, uint32_t conv,
    uint8_t flags, uint32_t timeout) {
  (void)connection;
  (void)conv;
  (void)flags;
  (void)timeout;
  assert(0 && "fixture tests the session-frame callback separately");
  return H2_PAL_ERR_INVALID_STATE;
}
/* SERIAL_DISCONNECT */
/* SERIAL_STAGE_MARKER */
/* SERIAL_STAGE */

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

static h2_pal_result_t fixture_sink(void* user, const uint8_t* data,
                                    size_t len) {
  h2_iostreamikcp_t* stream = user;
  (void)data;
  (void)len;
  if (stream->close_phase)
    return stream->fast_boot == 11 ? H2_PAL_ERR_CLOSED : H2_PAL_ERR_IO;
  return H2_PAL_OK;
}

static h2_pal_result_t reboot_timeout_sink(void* user, const uint8_t* data,
                                           size_t len) {
  (void)user;
  (void)data;
  (void)len;
  return H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t stage_sink(void* user, const uint8_t* data, size_t len) {
  h2_iostreamikcp_t* stream = user;
  (void)data;
  (void)len;
  if (stream->fast_boot == 14 || stream->fast_boot == 17)
    return H2_PAL_ERR_TIMEOUT;
  if (stream->fast_boot == 16) return H2_PAL_ERR_CLOSED;
  return H2_PAL_OK;
}

static h2_pal_result_t stage_payload(void* user, uint64_t offset, uint8_t* out,
                                     size_t capacity, size_t* count) {
  unsigned* reads = user;
  assert(offset == 0u && capacity >= 1u);
  ++*reads;
  out[0] = 0x42u;
  *count = 1u;
  return H2_PAL_OK;
}

int main(void) {
  (void)serial_status_diagnostics;
  (void)serial_stream_now_ms;
  (void)find_line_marker;
  for (int fast_boot = 0; fast_boot < 21; ++fast_boot) {
    clock_ms = 0u;
    accept_count = 0u;
    freed = 0u;
    h2_iostreamikcp_t stream = {.fast_boot = fast_boot};
    h2_iostreamikcp_filter_init(&stream.filter);
    const h2_pal_time_vtable_t time_vtable = {.get_monotonic_ms = monotonic};
    const h2_pal_time_api_t time = {.vtable = &time_vtable};
    test_config_t config = {0};
    if (fast_boot == 20) config.on_log = reboot_timeout_sink;
    if (fast_boot == 10 || fast_boot == 11) {
      config.on_log = fixture_sink;
      config.log_user = &stream;
    }
    test_result_t evidence = {0};
    h2_e2e_transport_context_t context = {
        .config = &config,
        .case_result = &evidence,
        .monitor_reboot = 1u,
    };
    const h2_pal_mem_api_t memory = {.vtable = &memory_vtable};
    const h2_pal_serial_host_api_t serial = {.vtable = &serial_vtable};
    h2_h2loader_host_serial_connection_t connection = {
        .serial = &serial,
        .allocator = &memory,
        .session = (h2_pal_serial_host_session_t*)&stream,
        .time = &time,
        .stream = &stream,
        .on_log = monitor_output,
        .log_user = &context,
        .command_timeout_ms = 1000u,
    };
    stream.log_user = &connection;
    if (fast_boot >= 14 && fast_boot <= 18) {
      connection.on_log = stage_sink;
      connection.log_user = &stream;
      h2_h2loader_host_catalog_entry_t asset = {
          .operation = H2_H2LOADER_HOST_ASSET_OPERATION_MANAGED_INSTALL,
          .role = H2_H2LOADER_HOST_ASSET_ROLE_APP,
          .bytes = 1u,
      };
      memset(asset.sha256, '0', 64u);
      asset.sha256[64] = '\0';
      unsigned reads = 0u;
      h2_pal_result_t rc = h2_h2loader_host_serial_stage(
          &connection, &asset, stage_payload, &reads, NULL, NULL, NULL, NULL);
      h2_pal_result_t expected =
          (fast_boot == 14 || fast_boot == 17)
              ? H2_PAL_ERR_TIMEOUT
              : (fast_boot == 16 ? H2_PAL_ERR_IO : H2_PAL_OK);
      assert(rc == expected && reads == 1u);
      if (expected != H2_PAL_OK) {
        unsigned writes = stream.writes;
        assert(h2_h2loader_host_serial_stage(&connection, &asset, stage_payload,
                                             &reads, NULL, NULL, NULL,
                                             NULL) == expected);
        assert(h2_h2loader_host_serial_activate(&connection, &asset) ==
               expected);
        assert(reads == 1u && stream.writes == writes);
      }
      h2_h2loader_host_serial_connection_t* owned = &connection;
      uint32_t resets = 99u;
      assert(h2_h2loader_host_serial_disconnect_with_reset(&owned, &resets) ==
             expected);
      assert(owned == NULL && freed == 1u && resets == 0u);
      continue;
    }
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
    h2_pal_result_t command_rc =
        h2_h2loader_host_serial_execute_command(&connection, &request, &result);
    if (fast_boot == 20) {
      assert(command_rc == H2_PAL_ERR_TIMEOUT);
      assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
      assert(result.transport_result == H2_PAL_ERR_TIMEOUT &&
             accept_count == 1u);
      assert(stream.flush_closed_once ==
             0); /* A failing sink forbids ACK-grace I/O. */
      h2_h2loader_host_serial_connection_t* owned = &connection;
      uint32_t resets = 99u;
      assert(h2_h2loader_host_serial_disconnect_with_reset(&owned, &resets) ==
             H2_PAL_ERR_TIMEOUT);
      assert(owned == NULL && freed == 1u && stream.stream_closed &&
             resets == 0u);
      continue;
    }
    assert(command_rc == H2_PAL_OK);
    assert(result.terminal == H2_H2LOADER_HOST_COMMAND_TERMINAL_OK);
    assert(accept_count == 1u && context.monitor_logs);
    assert(result.accepted_link_closed ==
           (fast_boot != 0 && fast_boot != 6 && fast_boot != 8 &&
            fast_boot != 10 && fast_boot != 11));
    assert(context.monitor_output_bytes ==
           ((fast_boot == 1 || fast_boot == 2 || fast_boot == 5 ||
             fast_boot == 7 || fast_boot == 9 || fast_boot == 12 ||
             fast_boot == 13 || fast_boot == 19)
                ? sizeof(boot) - 1u
                : 0u));
    unsigned polls = 4u;
    assert(h2_h2loader_host_serial_monitor_logs(&connection, bounded, &polls) ==
           H2_PAL_EXIT);
    /* FINAL is drained without becoming evidence, including in ACK grace. */
    assert(stream.offset == stream.length);
    assert(context.monitor_output_bytes ==
           ((fast_boot == 1 || fast_boot == 2 || fast_boot == 5 ||
             fast_boot == 7 || fast_boot == 9 || fast_boot == 12 ||
             fast_boot == 13 || fast_boot == 19)
                ? sizeof(boot) - 1u
                : 0u));
    if (fast_boot == 8) {
      session_ack_context_t closing = {.closing_connection = &connection};
      const h2_iostreamikcp_frame_t frame = {
          .flags = H2_IOSTREAMIKCP_FRAME_FLAG_DATA,
          .payload = (const uint8_t*)boot,
          .payload_len = sizeof(boot) - 1u,
      };
      assert(session_frame(&closing, &frame) == H2_PAL_ERR_CLOSED);
    }
    stream.close_phase = 1;
    h2_h2loader_host_serial_connection_t* owned = &connection;
    uint32_t resets = 0u;
    h2_pal_result_t close_rc =
        h2_h2loader_host_serial_disconnect_with_reset(&owned, &resets);
    assert(close_rc ==
           ((fast_boot == 10 || fast_boot == 11) ? H2_PAL_ERR_IO : H2_PAL_OK));
    assert(owned == NULL && freed == 1u && stream.stream_closed);
    assert(resets == (fast_boot == 0 ? 0u
                                     : ((fast_boot == 7 || fast_boot == 9 ||
                                         fast_boot == 12 || fast_boot == 13 ||
                                         fast_boot == 19)
                                            ? 2u
                                            : 1u)));
    if (fast_boot == 6 || fast_boot == 8)
      assert(context.monitor_output_bytes == sizeof(boot) - 1u);
    if (fast_boot == 7 || fast_boot == 9 || fast_boot == 12 ||
        fast_boot == 13 || fast_boot == 19)
      assert(context.monitor_output_bytes == 2u * (sizeof(boot) - 1u));
    if (fast_boot == 9 || fast_boot == 12 || fast_boot == 13 || fast_boot == 19)
      assert(stream.filter.len == 0u && stream.offset == stream.length);
  }
  return 0;
}
