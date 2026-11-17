#include "h2_alipay_trace.h"
#include <assert.h>
#include <string.h>
int main(void) {
  static h2_alipay_trace_t trace;
  h2_alipay_trace_event_t in = {
      .call_id = 7, .parent_id = 2, .connection_id = 3};
  h2_alipay_trace_event_t out;
  unsigned char raw[256];
  memset(raw, 0x5a, sizeof(raw));
  h2_alipay_trace_reset(&trace, false);
  h2_alipay_trace_push(&trace, &in, raw, sizeof(raw));
  assert(h2_alipay_trace_next(&trace, &out));
  assert(out.sequence == 1 && out.call_id == 7 && out.parent_id == 2);
  assert(out.connection_id == 3 && out.length == 256 && out.captured == 0);
  assert(out.bytes[0] == 0 && !h2_alipay_trace_next(&trace, &out));
  h2_alipay_trace_reset(&trace, true);
  for (unsigned i = 0; i < H2_ALIPAY_TRACE_EVENTS + 3; ++i)
    h2_alipay_trace_push(&trace, &in, raw, sizeof(raw));
  assert(trace.dropped == 3 &&
         trace.truncated_bytes == 128 * H2_ALIPAY_TRACE_EVENTS);
  for (unsigned i = 0; i < H2_ALIPAY_TRACE_EVENTS; ++i) {
    assert(h2_alipay_trace_next(&trace, &out));
    assert(out.sequence == i + 1 && out.captured == 128);
    assert(memcmp(out.bytes, raw, 128) == 0);
  }
  assert(!h2_alipay_trace_next(&trace, &out));
  return 0;
}
