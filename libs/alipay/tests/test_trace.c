#include "h2_alipay_trace.h"
#include "h2_alipay.h"
#include <assert.h>
#include <string.h>
static int init_calls, init_result;
static int initialize(void) {
  ++init_calls;
  return init_result;
}
int main(void) {
  for (int failure = 0; failure < 2; ++failure) {
    h2_alipay_initialization_t state = {0};
    int sdk_rc;
    bool first;
    init_result = failure ? -57 : 0;
    int before = init_calls;
    assert(h2_alipay_initialize_once(&state, initialize, &sdk_rc, &first) == 0);
    assert(first && sdk_rc == init_result && init_calls == before + 1);
    assert(h2_alipay_initialize_once(&state, initialize, &sdk_rc, &first) == 0);
    assert(!first && sdk_rc == init_result && init_calls == before + 1);
  }
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
