#ifndef H2_BK_UART_TUNNEL_CODEC_H
#define H2_BK_UART_TUNNEL_CODEC_H

#include <stddef.h>
#include <stdint.h>

#define H2_BK_UART_TUNNEL_MAX_FRAME_SIZE 1042u
#define H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE 96u
#define H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE 10u
#define H2_BK_UART_TUNNEL_RECORD_BUFFER_SIZE                                   \
  (H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE + H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE)

typedef enum h2_bk_uart_tunnel_record_kind {
  H2_BK_UART_TUNNEL_RECORD_BEGIN = 1,
  H2_BK_UART_TUNNEL_RECORD_DATA = 2,
  H2_BK_UART_TUNNEL_RECORD_END = 3,
} h2_bk_uart_tunnel_record_kind_t;

typedef int (*h2_bk_uart_tunnel_frame_fn)(void *user, uint16_t sequence,
                                          const uint8_t *data, size_t len);

typedef struct h2_bk_uart_tunnel_decoder {
  uint8_t record[H2_BK_UART_TUNNEL_RECORD_BUFFER_SIZE];
  size_t record_length;
  uint8_t frame[H2_BK_UART_TUNNEL_MAX_FRAME_SIZE];
  size_t frame_length;
  size_t declared_length;
  uint16_t sequence;
  int escaped;
  int discard_record;
  int transaction_active;
} h2_bk_uart_tunnel_decoder_t;

void h2_bk_uart_tunnel_decoder_init(h2_bk_uart_tunnel_decoder_t *decoder);
int h2_bk_uart_tunnel_decoder_input(h2_bk_uart_tunnel_decoder_t *decoder,
                                    const uint8_t *data, size_t len,
                                    h2_bk_uart_tunnel_frame_fn on_frame,
                                    void *user);

#endif
