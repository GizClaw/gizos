#include "h2_bk_uart_tunnel_codec.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#define SLIP_END 0xc0u
#define SLIP_ESC 0xdbu
#define SLIP_ESC_END 0xdcu
#define SLIP_ESC_ESC 0xddu

typedef struct capture {
  uint8_t frames[4][H2_BK_UART_TUNNEL_MAX_FRAME_SIZE];
  size_t lengths[4];
  size_t count;
  int callback_result;
} capture_t;

static int capture_frame(void *user, uint16_t sequence, const uint8_t *data,
                         size_t len) {
  capture_t *capture = user;
  (void)sequence;
  assert(capture != NULL);
  assert(capture->count < 4u);
  assert(len <= sizeof(capture->frames[0]));
  memcpy(capture->frames[capture->count], data, len);
  capture->lengths[capture->count] = len;
  ++capture->count;
  return capture->callback_result;
}

static void write_le16(uint8_t out[2], size_t value) {
  out[0] = (uint8_t)(value & 0xffu);
  out[1] = (uint8_t)((value >> 8u) & 0xffu);
}

static size_t append_record(uint8_t *out, size_t capacity, uint8_t kind,
                            uint16_t sequence, size_t total, size_t offset,
                            const uint8_t *payload, size_t payload_length) {
  uint8_t raw[H2_BK_UART_TUNNEL_RECORD_BUFFER_SIZE];
  assert(payload_length <= H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE);
  raw[0] = 1u;
  raw[1] = kind;
  write_le16(raw + 2u, sequence);
  write_le16(raw + 4u, total);
  write_le16(raw + 6u, offset);
  write_le16(raw + 8u, payload_length);
  if (payload_length != 0u) {
    memcpy(raw + H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE, payload, payload_length);
  }
  size_t used = 0u;
  assert(used < capacity);
  out[used++] = SLIP_END;
  size_t raw_length = H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE + payload_length;
  for (size_t i = 0u; i < raw_length; ++i) {
    assert(used + 2u < capacity);
    if (raw[i] == SLIP_END) {
      out[used++] = SLIP_ESC;
      out[used++] = SLIP_ESC_END;
    } else if (raw[i] == SLIP_ESC) {
      out[used++] = SLIP_ESC;
      out[used++] = SLIP_ESC_ESC;
    } else {
      out[used++] = raw[i];
    }
  }
  out[used++] = SLIP_END;
  return used;
}

static size_t append_transaction(uint8_t *out, size_t capacity,
                                 uint16_t sequence, const uint8_t *payload,
                                 size_t payload_length) {
  size_t used = append_record(out, capacity, H2_BK_UART_TUNNEL_RECORD_BEGIN,
                              sequence, payload_length, 0u, NULL, 0u);
  size_t offset = 0u;
  while (offset < payload_length) {
    size_t fragment_length = payload_length - offset;
    if (fragment_length > H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE) {
      fragment_length = H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE;
    }
    used += append_record(
        out + used, capacity - used, H2_BK_UART_TUNNEL_RECORD_DATA, sequence,
        payload_length, offset, payload + offset, fragment_length);
    offset += fragment_length;
  }
  used +=
      append_record(out + used, capacity - used, H2_BK_UART_TUNNEL_RECORD_END,
                    sequence, payload_length, payload_length, NULL, 0u);
  return used;
}

static void test_binary_fragmentation_and_back_to_back(void) {
  static const uint8_t first[] = {0u, 1u, SLIP_END, 2u, SLIP_ESC, 3u};
  static const uint8_t second[] = {5u, 0u, 6u};
  uint8_t encoded[256];
  size_t used =
      append_transaction(encoded, sizeof(encoded), 1u, first, sizeof(first));
  used += append_transaction(encoded + used, sizeof(encoded) - used, 2u, second,
                             sizeof(second));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);

  for (size_t i = 0u; i < used; ++i) {
    assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded + i, 1u,
                                           capture_frame, &capture) == 0);
  }
  assert(capture.count == 2u);
  assert(capture.lengths[0] == sizeof(first));
  assert(memcmp(capture.frames[0], first, sizeof(first)) == 0);
  assert(capture.lengths[1] == sizeof(second));
  assert(memcmp(capture.frames[1], second, sizeof(second)) == 0);
}

static void test_maximum_frame(void) {
  uint8_t payload[H2_BK_UART_TUNNEL_MAX_FRAME_SIZE];
  uint8_t encoded[4096];
  for (size_t i = 0u; i < sizeof(payload); ++i) {
    payload[i] = (uint8_t)i;
  }
  size_t used = append_transaction(encoded, sizeof(encoded), 0xffffu, payload,
                                   sizeof(payload));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(capture.lengths[0] == sizeof(payload));
  assert(memcmp(capture.frames[0], payload, sizeof(payload)) == 0);
}

static void test_malformed_transaction_does_not_pollute_next(void) {
  static const uint8_t stale[] = {1u, 2u, 3u};
  static const uint8_t fresh[] = {9u, 8u, 7u, 6u};
  uint8_t encoded[512];
  size_t used =
      append_record(encoded, sizeof(encoded), H2_BK_UART_TUNNEL_RECORD_BEGIN,
                    3u, sizeof(stale), 0u, NULL, 0u);
  used += append_record(encoded + used, sizeof(encoded) - used,
                        H2_BK_UART_TUNNEL_RECORD_DATA, 3u, sizeof(stale), 1u,
                        stale, sizeof(stale));
  used += append_transaction(encoded + used, sizeof(encoded) - used, 4u, fresh,
                             sizeof(fresh));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(capture.lengths[0] == sizeof(fresh));
  assert(memcmp(capture.frames[0], fresh, sizeof(fresh)) == 0);
}

static void test_duplicate_fragment_drops_transaction(void) {
  static const uint8_t stale[] = {1u, 2u, 3u, 4u};
  static const uint8_t fresh[] = {5u, 6u};
  uint8_t encoded[512];
  size_t used =
      append_record(encoded, sizeof(encoded), H2_BK_UART_TUNNEL_RECORD_BEGIN,
                    8u, sizeof(stale), 0u, NULL, 0u);
  used += append_record(encoded + used, sizeof(encoded) - used,
                        H2_BK_UART_TUNNEL_RECORD_DATA, 8u, sizeof(stale), 0u,
                        stale, 2u);
  used += append_record(encoded + used, sizeof(encoded) - used,
                        H2_BK_UART_TUNNEL_RECORD_DATA, 8u, sizeof(stale), 0u,
                        stale, 2u);
  used += append_record(encoded + used, sizeof(encoded) - used,
                        H2_BK_UART_TUNNEL_RECORD_END, 8u, sizeof(stale),
                        sizeof(stale), NULL, 0u);
  used += append_transaction(encoded + used, sizeof(encoded) - used, 9u, fresh,
                             sizeof(fresh));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(capture.lengths[0] == sizeof(fresh));
  assert(memcmp(capture.frames[0], fresh, sizeof(fresh)) == 0);
}

static void test_oversize_begin_is_rejected(void) {
  static const uint8_t payload[] = {9u};
  uint8_t encoded[256];
  size_t used =
      append_record(encoded, sizeof(encoded), H2_BK_UART_TUNNEL_RECORD_BEGIN,
                    10u, H2_BK_UART_TUNNEL_MAX_FRAME_SIZE + 1u, 0u, NULL, 0u);
  used += append_transaction(encoded + used, sizeof(encoded) - used, 11u,
                             payload, sizeof(payload));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(memcmp(capture.frames[0], payload, sizeof(payload)) == 0);
}

static void test_partial_record_and_invalid_escape_resync(void) {
  static const uint8_t payload[] = {4u, 5u, 6u};
  uint8_t encoded[256] = {SLIP_END, 1u, 2u, SLIP_ESC, 0x01u, SLIP_END};
  size_t used = 6u;
  used += append_transaction(encoded + used, sizeof(encoded) - used, 5u,
                             payload, sizeof(payload));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(memcmp(capture.frames[0], payload, sizeof(payload)) == 0);
}

static void test_reset_drops_incomplete_transaction(void) {
  static const uint8_t payload[] = {7u, 8u};
  uint8_t encoded[256];
  size_t first_record =
      append_record(encoded, sizeof(encoded), H2_BK_UART_TUNNEL_RECORD_BEGIN,
                    6u, 3u, 0u, NULL, 0u);
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {0};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, first_record,
                                         capture_frame, &capture) == 0);
  h2_bk_uart_tunnel_decoder_init(&decoder);
  size_t used = append_transaction(encoded, sizeof(encoded), 1u, payload,
                                   sizeof(payload));
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == 0);
  assert(capture.count == 1u);
  assert(memcmp(capture.frames[0], payload, sizeof(payload)) == 0);
}

static void test_callback_error_is_propagated_and_state_is_reset(void) {
  static const uint8_t payload[] = {1u};
  uint8_t encoded[128];
  size_t used = append_transaction(encoded, sizeof(encoded), 7u, payload,
                                   sizeof(payload));
  h2_bk_uart_tunnel_decoder_t decoder;
  capture_t capture = {.callback_result = -7};
  h2_bk_uart_tunnel_decoder_init(&decoder);
  assert(h2_bk_uart_tunnel_decoder_input(&decoder, encoded, used, capture_frame,
                                         &capture) == -7);
  assert(!decoder.transaction_active);
}

int main(void) {
  test_binary_fragmentation_and_back_to_back();
  test_maximum_frame();
  test_malformed_transaction_does_not_pollute_next();
  test_duplicate_fragment_drops_transaction();
  test_oversize_begin_is_rejected();
  test_partial_record_and_invalid_escape_resync();
  test_reset_drops_incomplete_transaction();
  test_callback_error_is_propagated_and_state_is_reset();
  return 0;
}
