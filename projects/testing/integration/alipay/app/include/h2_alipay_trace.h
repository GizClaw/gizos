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
} h2_alipay_trace_t;

void h2_alipay_trace_reset(h2_alipay_trace_t* trace, bool private_raw);
/** event is copied; data may be NULL only when length is zero. */
void h2_alipay_trace_push(h2_alipay_trace_t* trace,
                          const h2_alipay_trace_event_t* event,
                          const void* data, size_t length);
bool h2_alipay_trace_next(h2_alipay_trace_t* trace,
                          h2_alipay_trace_event_t* event);
/** Begin a distinct export using a boot-random nonce and caller-owned sequence
 * initialized to zero once per boot. Writes 32 lowercase hex digits plus NUL.
 * Call only for a new export; summary replays retain the last ID. Independent
 * of trace reset/draining. Serialized, synchronous, with no allocation or IO.
 * All pointers are required and borrowed only for this call. Returns false
 * without changing outputs for invalid arguments or an exhausted sequence. */
bool h2_alipay_trace_capture_begin(const uint8_t boot_nonce[16],
                                   uint64_t* sequence, char run_id[33]);
#ifdef __cplusplus
}
#endif
#endif
