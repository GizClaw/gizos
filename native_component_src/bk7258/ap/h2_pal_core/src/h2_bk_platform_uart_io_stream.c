#include "h2_bk_platform_core.h"

#include "driver/mb_uart_driver.h"
#include "os/os.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define H2_BK_UART_TUNNEL_MAX_FRAME_SIZE 1042u
#define H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE 96u
#define H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE 10u
#define H2_BK_UART_TUNNEL_MAX_ENCODED_RECORD_SIZE 214u
#define H2_BK_UART_TUNNEL_SLIP_END 0xc0u
#define H2_BK_UART_TUNNEL_SLIP_ESC 0xdbu
#define H2_BK_UART_TUNNEL_SLIP_ESC_END 0xdcu
#define H2_BK_UART_TUNNEL_SLIP_ESC_ESC 0xddu
#define H2_BK_UART_TUNNEL_VERSION 1u
#define H2_BK_UART_TUNNEL_RECORD_BEGIN 1u
#define H2_BK_UART_TUNNEL_RECORD_DATA 2u
#define H2_BK_UART_TUNNEL_RECORD_END 3u
#define H2_BK_UART_BAUD_RATE 230400u
#define H2_BK_UART_TX_ACK_MAGIC 0xa5u
#define H2_BK_UART_TX_ACK_VERSION 1u

static volatile int s_initialized;
static uint16_t s_sequence;

static uint32_t elapsed_ms(uint32_t started) {
  return (uint32_t)rtos_get_time() - started;
}

static h2_pal_result_t configure(void *user,
                                 const h2_pal_uart_io_stream_config_t *config) {
  (void)user;
  if (config == NULL || config->baud_rate != H2_BK_UART_BAUD_RATE ||
      config->data_bits != 8u || config->stop_bits != 1u ||
      config->parity != H2_PAL_UART_PARITY_NONE ||
      config->flow_control != H2_PAL_UART_FLOW_CONTROL_NONE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (!s_initialized) {
    if (bk_mb_uart_dev_init(MB_UART0) != 0) {
      return H2_PAL_ERR_IO;
    }
    if (bk_mb_uart_dev_init(MB_UART1) != 0) {
      (void)bk_mb_uart_dev_deinit(MB_UART0);
      return H2_PAL_ERR_IO;
    }
    s_sequence = 0u;
    s_initialized = 1;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t read_stream(void *user, void *buffer, size_t len,
                                   size_t *out_read, uint32_t timeout_ms) {
  (void)user;
  if (!s_initialized || buffer == NULL || out_read == NULL ||
      len > UINT16_MAX) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_read = 0u;
  uint32_t started = (uint32_t)rtos_get_time();
  do {
    if (!s_initialized) {
      return H2_PAL_ERR_CLOSED;
    }
    uint16_t count = bk_mb_uart_read(MB_UART0, buffer, (uint16_t)len);
    if (count != 0u) {
      *out_read = count;
      return H2_PAL_OK;
    }
    if (timeout_ms == 0u) {
      break;
    }
    rtos_delay_milliseconds(1u);
  } while (elapsed_ms(started) < timeout_ms);
  return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t mailbox_write_all(const uint8_t *data, size_t len,
                                         uint32_t started,
                                         uint32_t timeout_ms) {
  while (len != 0u) {
    if (!s_initialized) {
      return H2_PAL_ERR_CLOSED;
    }
    uint16_t take = len > UINT16_MAX ? UINT16_MAX : (uint16_t)len;
    uint16_t written = bk_mb_uart_write(MB_UART0, (uint8_t *)data, take);
    if (written != 0u) {
      data += written;
      len -= written;
      continue;
    }
    if (timeout_ms == 0u || elapsed_ms(started) >= timeout_ms) {
      return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
    }
    rtos_delay_milliseconds(1u);
  }
  return H2_PAL_OK;
}

static void write_le16(uint8_t out[2], uint16_t value) {
  out[0] = (uint8_t)(value & 0xffu);
  out[1] = (uint8_t)(value >> 8u);
}

static h2_pal_result_t wait_for_tx_ack(uint16_t sequence, uint32_t started,
                                       uint32_t timeout_ms) {
  uint8_t state = 0u;
  uint8_t sequence_low = 0u;
  do {
    if (!s_initialized) {
      return H2_PAL_ERR_CLOSED;
    }
    uint8_t value = 0u;
    if (bk_mb_uart_read(MB_UART1, &value, sizeof(value)) != 0u) {
      if (state == 0u) {
        state = value == H2_BK_UART_TX_ACK_MAGIC ? 1u : 0u;
      } else if (state == 1u) {
        state = value == H2_BK_UART_TX_ACK_VERSION ? 2u : 0u;
      } else if (state == 2u) {
        sequence_low = value;
        state = 3u;
      } else {
        uint16_t ack_sequence =
            (uint16_t)sequence_low | ((uint16_t)value << 8u);
        if (ack_sequence == sequence) {
          return H2_PAL_OK;
        }
        state = 0u;
      }
      continue;
    }
    if (timeout_ms == 0u || elapsed_ms(started) >= timeout_ms) {
      break;
    }
    rtos_delay_milliseconds(1u);
  } while (elapsed_ms(started) < timeout_ms);
  return timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK : H2_PAL_ERR_TIMEOUT;
}

static h2_pal_result_t write_record(uint8_t kind, uint16_t sequence,
                                    size_t declared_length, size_t offset,
                                    const uint8_t *payload,
                                    size_t payload_length, uint32_t started,
                                    uint32_t timeout_ms) {
  uint8_t raw[H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE +
              H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE];
  uint8_t encoded[H2_BK_UART_TUNNEL_MAX_ENCODED_RECORD_SIZE];
  if (declared_length > UINT16_MAX || offset > UINT16_MAX ||
      payload_length > H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE ||
      (payload == NULL && payload_length != 0u)) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  raw[0] = H2_BK_UART_TUNNEL_VERSION;
  raw[1] = kind;
  write_le16(raw + 2u, sequence);
  write_le16(raw + 4u, (uint16_t)declared_length);
  write_le16(raw + 6u, (uint16_t)offset);
  write_le16(raw + 8u, (uint16_t)payload_length);
  if (payload_length != 0u) {
    memcpy(raw + H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE, payload, payload_length);
  }

  size_t encoded_length = 0u;
  encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_END;
  size_t raw_length = H2_BK_UART_TUNNEL_RECORD_HEADER_SIZE + payload_length;
  for (size_t i = 0u; i < raw_length; ++i) {
    if (raw[i] == H2_BK_UART_TUNNEL_SLIP_END) {
      encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_ESC;
      encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_ESC_END;
    } else if (raw[i] == H2_BK_UART_TUNNEL_SLIP_ESC) {
      encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_ESC;
      encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_ESC_ESC;
    } else {
      encoded[encoded_length++] = raw[i];
    }
  }
  encoded[encoded_length++] = H2_BK_UART_TUNNEL_SLIP_END;
  return mailbox_write_all(encoded, encoded_length, started, timeout_ms);
}

static h2_pal_result_t write_stream(void *user, const void *buffer, size_t len,
                                    size_t *out_written, uint32_t timeout_ms) {
  (void)user;
  if (!s_initialized || buffer == NULL || out_written == NULL || len == 0u ||
      len > H2_BK_UART_TUNNEL_MAX_FRAME_SIZE) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  *out_written = 0u;
  ++s_sequence;
  if (s_sequence == 0u) {
    ++s_sequence;
  }
  uint16_t sequence = s_sequence;
  uint32_t started = (uint32_t)rtos_get_time();
  h2_pal_result_t rc = write_record(H2_BK_UART_TUNNEL_RECORD_BEGIN, sequence,
                                    len, 0u, NULL, 0u, started, timeout_ms);
  size_t offset = 0u;
  while (rc == H2_PAL_OK && offset < len) {
    size_t fragment_length = len - offset;
    if (fragment_length > H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE) {
      fragment_length = H2_BK_UART_TUNNEL_MAX_FRAGMENT_SIZE;
    }
    rc = write_record(H2_BK_UART_TUNNEL_RECORD_DATA, sequence, len, offset,
                      (const uint8_t *)buffer + offset, fragment_length,
                      started, timeout_ms);
    offset += fragment_length;
  }
  if (rc == H2_PAL_OK) {
    rc = write_record(H2_BK_UART_TUNNEL_RECORD_END, sequence, len, len, NULL,
                      0u, started, timeout_ms);
  }
  if (rc == H2_PAL_OK) {
    rc = wait_for_tx_ack(sequence, started, timeout_ms);
  }
  if (rc == H2_PAL_OK) {
    *out_written = len;
  }
  return rc;
}

static h2_pal_result_t flush_stream(void *user) {
  (void)user;
  if (!s_initialized) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  /* write_stream() does not return until CP acknowledges the transaction.
   * CP emits that acknowledgement only after the complete frame has left the
   * physical UART, so there is no outstanding transport work to wait for
   * here. bk_mb_uart_is_tx_over(MB_UART0) describes the AP-to-CP mailbox, not
   * the CP-owned physical UART, and can remain false after delivery. */
  return H2_PAL_OK;
}

static const h2_pal_uart_io_stream_vtable_t s_vtable = {
    .configure = configure,
    .read = read_stream,
    .write = write_stream,
    .flush = flush_stream,
};

static const h2_pal_uart_io_stream_api_t s_api = {
    .vtable = &s_vtable,
};

const h2_pal_uart_io_stream_api_t *h2_bk_platform_uart_io_stream_api(void) {
  return &s_api;
}

void h2_bk_platform_uart_io_stream_deinit(void) {
  if (s_initialized) {
    s_initialized = 0;
    (void)bk_mb_uart_dev_deinit(MB_UART1);
    (void)bk_mb_uart_dev_deinit(MB_UART0);
  }
}
