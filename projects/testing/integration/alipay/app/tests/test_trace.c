#include "h2_alipay_trace.h"
#include <assert.h>
#include <string.h>

static void test_successive_captures(void) {
  const uint8_t nonce[16] = {0u, 1u, 2u,  3u,  4u,  5u,  6u,  7u,
                             8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u};
  uint64_t sequence = 0u;
  char first[33], second[33];
  assert(h2_alipay_trace_capture_begin(nonce, &sequence, first));
  assert(sequence == 1u &&
         strcmp(first, "112ac280cda01f4cd59e4150e2be4230") == 0);
  assert(h2_alipay_trace_capture_begin(nonce, &sequence, second));
  assert(sequence == 2u &&
         strcmp(second, "8e87288ec670948426d95202777216b2") == 0 &&
         strcmp(first, second) != 0);
  sequence = 255u;
  assert(h2_alipay_trace_capture_begin(nonce, &sequence, second));
  assert(sequence == 256u &&
         strcmp(second, "e65a5c34f086ae550fa6512041d82010") == 0);
  sequence = UINT64_MAX - 1u;
  assert(h2_alipay_trace_capture_begin(nonce, &sequence, second));
  assert(sequence == UINT64_MAX &&
         strcmp(second, "ad8e2c696b06357421a0f8c3fc8d7401") == 0);
  memcpy(first, second, sizeof(first));
  assert(!h2_alipay_trace_capture_begin(nonce, &sequence, second));
  assert(sequence == UINT64_MAX && memcmp(first, second, sizeof(first)) == 0);
  sequence = 0u;
  assert(!h2_alipay_trace_capture_begin(NULL, &sequence, second));
  assert(!h2_alipay_trace_capture_begin(nonce, NULL, second));
  assert(!h2_alipay_trace_capture_begin(nonce, &sequence, NULL));
  assert(sequence == 0u && memcmp(first, second, sizeof(first)) == 0);
}

int main(void) {
  test_successive_captures();
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
