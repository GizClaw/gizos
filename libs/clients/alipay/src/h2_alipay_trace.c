#include "h2_alipay_trace.h"
#include "h2_sha256.h"
#include <limits.h>
#include <string.h>

static int diagnostic_class(const h2_alipay_trace_event_t* event) {
  if (event->result != 0) return -1;
  static const char* const operations[] = {"vendor.log", "clock.get_utc",
                                           "memory.alloc"};
  for (unsigned index = 0; index < H2_ALIPAY_TRACE_DIAGNOSTIC_CLASSES; ++index)
    if (strncmp(event->operation, operations[index],
                sizeof(event->operation)) == 0)
      return (int)index;
  return -1;
}

void h2_alipay_trace_reset(h2_alipay_trace_t* trace, bool private_raw) {
  if (trace == NULL) return;
  memset(trace, 0, sizeof(*trace));
  trace->private_raw = private_raw;
}
void h2_alipay_trace_push(h2_alipay_trace_t* trace,
                          const h2_alipay_trace_event_t* event,
                          const void* data, size_t length) {
  if (trace == NULL || event == NULL || (data == NULL && length != 0u)) return;
  int diagnostic = diagnostic_class(event);
  if (trace->diagnostic_limit != 0 && diagnostic >= 0) {
    uint32_t* seen = &trace->diagnostic_seen[diagnostic];
    bool retain = *seen < trace->diagnostic_limit;
    if (*seen != UINT32_MAX) ++*seen;
    if (!retain) {
      uint32_t* summarized = &trace->diagnostic_summarized[diagnostic];
      if (*summarized != UINT32_MAX) ++*summarized;
      return;
    }
  }
  ++trace->sequence;
  if (trace->count == H2_ALIPAY_TRACE_EVENTS) {
    if (trace->dropped != UINT32_MAX) ++trace->dropped;
    return;
  }
  h2_alipay_trace_event_t* dest = &trace->events[trace->count++];
  *dest = *event;
  dest->sequence = trace->sequence;
  dest->length = length;
  dest->captured =
      trace->private_raw
          ? (length < H2_ALIPAY_TRACE_BYTES ? length : H2_ALIPAY_TRACE_BYTES)
          : 0u;
  memset(dest->bytes, 0, sizeof(dest->bytes));
  h2_sha256_t sha;
  h2_sha256_init(&sha);
  h2_sha256_update(&sha, data, length);
  h2_sha256_finish(&sha, dest->digest);
  if (dest->captured != 0u) memcpy(dest->bytes, data, dest->captured);
  if (trace->private_raw) trace->truncated_bytes += length - dest->captured;
}
bool h2_alipay_trace_next(h2_alipay_trace_t* trace,
                          h2_alipay_trace_event_t* event) {
  if (trace == NULL || event == NULL || trace->read == trace->count)
    return false;
  *event = trace->events[trace->read++];
  return true;
}
