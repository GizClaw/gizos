#include "h2_bk_uart_tunnel_codec.h"

#include <string.h>

#define H2_BK_UART_TUNNEL_SLIP_END 0xc0u
#define H2_BK_UART_TUNNEL_SLIP_ESC 0xdbu
#define H2_BK_UART_TUNNEL_SLIP_ESC_END 0xdcu
#define H2_BK_UART_TUNNEL_SLIP_ESC_ESC 0xddu
#define H2_BK_UART_TUNNEL_VERSION 1u

static uint16_t read_le16(const uint8_t in[2]) {
  return (uint16_t)in[0] | ((uint16_t)in[1] << 8u);
}

static void reset_transaction(h2_bk_uart_tunnel_decoder_t *decoder) {
  decoder->frame_length = 0u;
  decoder->declared_length = 0u;
  decoder->sequence = 0u;
  decoder->transaction_active = 0;
}

void h2_bk_uart_tunnel_decoder_init(h2_bk_uart_tunnel_decoder_t *decoder) {
  if (decoder != NULL) {
    memset(decoder, 0, sizeof(*decoder));
  }
}

static int process_record(h2_bk_uart_tunnel_decoder_t *decoder,
                          h2_bk_uart_tunnel_frame_fn on_frame, void *user) {
  if (decoder->record_length < H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE) {
    reset_transaction(decoder);
    return 0;
  }
  const uint8_t *record = decoder->record;
  uint8_t kind = record[1];
  uint16_t sequence = read_le16(record + 2u);
  size_t declared_length = read_le16(record + 4u);
  size_t offset = read_le16(record + 6u);
  size_t payload_length = read_le16(record + 8u);
  if (record[0] != H2_BK_UART_TUNNEL_VERSION ||
      payload_length !=
          decoder->record_length - H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE ||
      payload_length > H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE) {
    reset_transaction(decoder);
    return 0;
  }

  if (kind == H2_BK_UART_TUNNEL_RECORD_BEGIN) {
    reset_transaction(decoder);
    if (sequence == 0u || declared_length == 0u ||
        declared_length > H2_BK_UART_TUNNEL_MAX_FRAME_SIZE || offset != 0u ||
        payload_length != 0u) {
      return 0;
    }
    decoder->sequence = sequence;
    decoder->declared_length = declared_length;
    decoder->transaction_active = 1;
    return 0;
  }

  if (!decoder->transaction_active || sequence != decoder->sequence ||
      declared_length != decoder->declared_length) {
    reset_transaction(decoder);
    return 0;
  }
  if (kind == H2_BK_UART_TUNNEL_RECORD_DATA) {
    if (payload_length == 0u || offset != decoder->frame_length ||
        payload_length > decoder->declared_length - decoder->frame_length) {
      reset_transaction(decoder);
      return 0;
    }
    memcpy(decoder->frame + decoder->frame_length,
           record + H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE, payload_length);
    decoder->frame_length += payload_length;
    return 0;
  }
  if (kind == H2_BK_UART_TUNNEL_RECORD_END && payload_length == 0u &&
      offset == decoder->declared_length &&
      decoder->frame_length == decoder->declared_length) {
    int rc = on_frame(user, decoder->sequence, decoder->frame,
                      decoder->frame_length);
    reset_transaction(decoder);
    return rc;
  }
  reset_transaction(decoder);
  return 0;
}

static int finish_record(h2_bk_uart_tunnel_decoder_t *decoder,
                         h2_bk_uart_tunnel_frame_fn on_frame, void *user) {
  int rc = 0;
  if (decoder->record_length != 0u) {
    if (decoder->discard_record || decoder->escaped) {
      reset_transaction(decoder);
    } else {
      rc = process_record(decoder, on_frame, user);
    }
  }
  decoder->record_length = 0u;
  decoder->escaped = 0;
  decoder->discard_record = 0;
  return rc;
}

int h2_bk_uart_tunnel_decoder_input(h2_bk_uart_tunnel_decoder_t *decoder,
                                    const uint8_t *data, size_t len,
                                    h2_bk_uart_tunnel_frame_fn on_frame,
                                    void *user) {
  if (decoder == NULL || on_frame == NULL || (data == NULL && len != 0u)) {
    return -1;
  }
  for (size_t i = 0u; i < len; ++i) {
    uint8_t value = data[i];
    if (value == H2_BK_UART_TUNNEL_SLIP_END) {
      int rc = finish_record(decoder, on_frame, user);
      if (rc != 0) {
        return rc;
      }
      continue;
    }
    if (decoder->discard_record) {
      continue;
    }
    if (decoder->escaped) {
      decoder->escaped = 0;
      if (value == H2_BK_UART_TUNNEL_SLIP_ESC_END) {
        value = H2_BK_UART_TUNNEL_SLIP_END;
      } else if (value == H2_BK_UART_TUNNEL_SLIP_ESC_ESC) {
        value = H2_BK_UART_TUNNEL_SLIP_ESC;
      } else {
        decoder->discard_record = 1;
        reset_transaction(decoder);
        continue;
      }
    } else if (value == H2_BK_UART_TUNNEL_SLIP_ESC) {
      decoder->escaped = 1;
      continue;
    }
    if (decoder->record_length == sizeof(decoder->record)) {
      decoder->discard_record = 1;
      reset_transaction(decoder);
      continue;
    }
    decoder->record[decoder->record_length++] = value;
  }
  return 0;
}
