#ifndef H2_BK_UART_CONTROL_TRACKER_H
#define H2_BK_UART_CONTROL_TRACKER_H

#include <stdint.h>

typedef struct h2_bk_uart_control_tracker {
  uint8_t state;
  uint8_t sequence_low;
  uint16_t pending_tx_ack;
  int pending_tx_ack_valid;
  int cp_ready;
} h2_bk_uart_control_tracker_t;

void h2_bk_uart_control_tracker_init(h2_bk_uart_control_tracker_t *tracker);
void h2_bk_uart_control_tracker_feed(h2_bk_uart_control_tracker_t *tracker,
                                    uint8_t value);
int h2_bk_uart_control_tracker_is_ready(
    const h2_bk_uart_control_tracker_t *tracker);
int h2_bk_uart_control_tracker_take_tx_ack(
    h2_bk_uart_control_tracker_t *tracker, uint16_t sequence);

#endif
