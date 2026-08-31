#include "h2_bk_uart_control_tracker.h"

#include <string.h>

#define H2_BK_UART_TX_ACK_MAGIC 0xa5u
#define H2_BK_UART_TX_ACK_VERSION 1u
#define H2_BK_UART_CP_READY_ACK 0x5bu

void h2_bk_uart_control_tracker_init(h2_bk_uart_control_tracker_t *tracker) {
  if (tracker != NULL) {
    memset(tracker, 0, sizeof(*tracker));
  }
}

void h2_bk_uart_control_tracker_feed(h2_bk_uart_control_tracker_t *tracker,
                                    uint8_t value) {
  if (tracker == NULL) {
    return;
  }
  if (tracker->state == 0u) {
    if (value == H2_BK_UART_CP_READY_ACK) {
      tracker->cp_ready = 1;
    } else if (value == H2_BK_UART_TX_ACK_MAGIC) {
      tracker->state = 1u;
    }
  } else if (tracker->state == 1u) {
    tracker->state = value == H2_BK_UART_TX_ACK_VERSION ? 2u : 0u;
  } else if (tracker->state == 2u) {
    tracker->sequence_low = value;
    tracker->state = 3u;
  } else {
    tracker->pending_tx_ack =
        (uint16_t)tracker->sequence_low | ((uint16_t)value << 8u);
    tracker->pending_tx_ack_valid = 1;
    tracker->state = 0u;
  }
}

int h2_bk_uart_control_tracker_is_ready(
    const h2_bk_uart_control_tracker_t *tracker) {
  return tracker != NULL && tracker->cp_ready;
}

int h2_bk_uart_control_tracker_take_tx_ack(
    h2_bk_uart_control_tracker_t *tracker, uint16_t sequence) {
  if (tracker == NULL || !tracker->pending_tx_ack_valid ||
      tracker->pending_tx_ack != sequence) {
    return 0;
  }
  tracker->pending_tx_ack_valid = 0;
  return 1;
}
