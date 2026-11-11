#ifndef H2_ALIPAY_TRACE_H
#define H2_ALIPAY_TRACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_ALIPAY_TRACE_EVENTS 512u
#define H2_ALIPAY_TRACE_BYTES 128u
#define H2_ALIPAY_TRACE_DIAGNOSTIC_CLASSES 3u
/** Fixed bounded runtime evidence. No dynamic allocation or IO on push.
 * One serialized producer; drain/reset only after that worker has joined.
 * Raw bytes are opt-in, prefixes are explicitly marked, and digest covers
 * observed bytes before capture truncation. Never treat wire bytes below
 * vendor TLS as decoded HTTP/RPC. IDs are unique within this trace context. */
typedef struct h2_alipay_trace_event {
  uint32_t sequence, call_id, parent_id, connection_id;
  uint64_t mono_ms, utc_ms, duration_ms;
  uint32_t wall_source;
  bool utc_valid;
  int result;
  uint32_t item;
  size_t length, captured;
  char operation[40], boundary[24], detail[80];
  uint8_t digest[32], bytes[H2_ALIPAY_TRACE_BYTES];
} h2_alipay_trace_event_t;

typedef struct h2_alipay_trace {
  h2_alipay_trace_event_t events[H2_ALIPAY_TRACE_EVENTS];
  size_t count, read;
  uint32_t sequence, next_call, dropped;
  size_t truncated_bytes;
  bool private_raw;
  /** Optional per-class sample budget: vendor.log, successful clock.get_utc,
   * successful memory.alloc. Zero retains every event. Summarized diagnostics
   * have explicit counters; SDK/HAL failures and other operations are retained
   * normally. Sequence numbers describe retained events, not summaries. */
  uint32_t diagnostic_limit;
  uint32_t diagnostic_seen[H2_ALIPAY_TRACE_DIAGNOSTIC_CLASSES];
  uint32_t diagnostic_summarized[H2_ALIPAY_TRACE_DIAGNOSTIC_CLASSES];
} h2_alipay_trace_t;

void h2_alipay_trace_reset(h2_alipay_trace_t* trace, bool private_raw);
/** event is copied; data may be NULL only when length is zero. */
void h2_alipay_trace_push(h2_alipay_trace_t* trace,
                          const h2_alipay_trace_event_t* event,
                          const void* data, size_t length);
bool h2_alipay_trace_next(h2_alipay_trace_t* trace,
                          h2_alipay_trace_event_t* event);
#ifdef __cplusplus
}
#endif
#endif
