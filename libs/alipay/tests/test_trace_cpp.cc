#include "h2_alipay.h"

int main() {
  h2_alipay_trace_t trace;
  h2_alipay_trace_reset(&trace, false);
  h2_alipay_trace_event_t event = {};
  h2_alipay_trace_push(&trace, &event, nullptr, 0);
  return h2_alipay_trace_next(&trace, &event) && event.sequence == 1 ? 0 : 1;
}
