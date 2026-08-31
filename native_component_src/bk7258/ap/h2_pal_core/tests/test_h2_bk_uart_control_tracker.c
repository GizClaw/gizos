#include "h2_bk_uart_control_tracker.h"

#include <assert.h>
#include <stddef.h>

static void test_ready_preserves_pending_tx_ack(void) {
  h2_bk_uart_control_tracker_t tracker;
  h2_bk_uart_control_tracker_init(&tracker);

  const uint8_t tx_ack[] = {0xa5u, 1u, 0x34u, 0x12u};
  for (size_t i = 0u; i < sizeof(tx_ack); ++i) {
    h2_bk_uart_control_tracker_feed(&tracker, tx_ack[i]);
  }
  h2_bk_uart_control_tracker_feed(&tracker, 0x5bu);

  assert(h2_bk_uart_control_tracker_is_ready(&tracker));
  assert(!h2_bk_uart_control_tracker_take_tx_ack(&tracker, 0x4321u));
  assert(h2_bk_uart_control_tracker_take_tx_ack(&tracker, 0x1234u));
  assert(!h2_bk_uart_control_tracker_take_tx_ack(&tracker, 0x1234u));
}

static void test_invalid_ack_version_recovers_to_ready(void) {
  h2_bk_uart_control_tracker_t tracker;
  h2_bk_uart_control_tracker_init(&tracker);

  h2_bk_uart_control_tracker_feed(&tracker, 0xa5u);
  h2_bk_uart_control_tracker_feed(&tracker, 2u);
  h2_bk_uart_control_tracker_feed(&tracker, 0x5bu);

  assert(h2_bk_uart_control_tracker_is_ready(&tracker));
  assert(!h2_bk_uart_control_tracker_take_tx_ack(&tracker, 0u));
}

int main(void) {
  test_ready_preserves_pending_tx_ack();
  test_invalid_ack_version_recovers_to_ready();
  return 0;
}
