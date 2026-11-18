#include "h2_fm175xx.h"
#include "h2_fm175xx_defs.h"
#include "h2_sha256.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

typedef enum card_state {
  ABSENT,
  IDLE,
  READY1,
  READY2,
  ACTIVE,
  HALT
} card_state_t;

typedef struct fixture {
  uint8_t registers[64];
  uint8_t tx[32];
  uint8_t rx[32];
  size_t tx_len;
  size_t rx_len;
  card_state_t card;
  uint8_t memory[1024];
  unsigned halts;
  unsigned wakes;
  unsigned reads;
  unsigned reads_after_writes;
  unsigned writes;
  unsigned reject_write;
  unsigned discard_write;
  bool discard_all_writes;
  uint8_t write_reply_bits, write_reply_low4, write_error;
  uint8_t write_reply_tail[2];
  bool write_timeout;
  bool clear_error_on_idle;
  bool write_reply_crc, corrupt_write_reply_crc;
  int fail_register;
  uint8_t silence_irq;
  uint8_t silence_error;
  uint8_t response_irq;
  int short_atqa;
  int unexpected_halt_reply;
  int no_initial_tx_alert;
  int reject_rf_enable;
  int reject_receiver_select;
  int reject_rf_gain;
  int idle_requires_reqa;
  int empty_before_reply;
  int truncate_fifo_write;
  uint8_t pending_rx[32];
  size_t pending_rx_len;
  unsigned delayed_replies;
  uint32_t reply_delay_us;
  uint32_t frame_elapsed_us;
  unsigned soft_resets;
  uint8_t reset_gsn;
  uint8_t reset_gwgsp;
  uint8_t empty_last_bits;
  uint8_t rx_last_bits;
  uint8_t pending_last_bits;
  int reply_on_empty_probe;
  int sak_crc_bytes;
  int corrupt_sak_crc;
  int short_sak_bits;
  int fail_post_idle;
  int read_crc_bytes;
  int corrupt_read_crc;
  uint8_t last_readable_page;
  unsigned rejected_reads;
  uint8_t read_error, read_error_page;
  uint8_t partial_read_bits;
  unsigned read_error_after_writes;
} fixture_t;

static uint16_t reply_crc(const uint8_t* bytes, size_t length) {
  uint16_t crc = 0x6363u;
  for (size_t index = 0u; index < length; ++index) {
    uint8_t value = (uint8_t)(bytes[index] ^ (uint8_t)crc);
    value ^= (uint8_t)(value << 4u);
    crc = (uint16_t)((crc >> 8u) ^ ((uint16_t)value << 8u) ^
                     ((uint16_t)value << 3u) ^ (value >> 4u));
  }
  return crc;
}

static void exchange(fixture_t* f) {
  f->registers[H2_FM175XX_REG_COMIRQ] = f->silence_irq ? f->silence_irq : 1u;
  f->registers[H2_FM175XX_REG_ERROR] = f->silence_error;
  f->rx_len = 0u;
  f->rx_last_bits = 0u;
  if ((f->registers[H2_FM175XX_REG_TX_CTRL] & 0x03u) != 0x03u ||
      (f->registers[H2_FM175XX_REG_RX_SEL] & 0xc0u) != 0x80u)
    return;
  if (f->tx_len == 2u && f->tx[0] == 0x50u && f->tx[1] == 0u) {
    assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
    assert((f->registers[H2_FM175XX_REG_RX_MODE] & 0x80u) == 0u);
    ++f->halts;
    if (f->card == ACTIVE) f->card = HALT;
    /* ISO14443A HLTA deliberately has no reply. */
    if (f->unexpected_halt_reply) {
      f->rx[0] = 0x0au;
      f->rx_len = 1u;
      f->registers[H2_FM175XX_REG_COMIRQ] = 0x20u;
    }
    return;
  }
  if (f->card == ABSENT) return;
  if (f->tx_len == 1u &&
      ((f->tx[0] == 0x26u && f->card == IDLE) ||
       (f->tx[0] == 0x52u &&
        ((f->card == IDLE && !f->idle_requires_reqa) || f->card == HALT)))) {
    ++f->wakes;
    f->card = READY1;
    f->rx[0] = 0x44u;
    f->rx[1] = 0u;
    f->rx_len = f->short_atqa ? 1u : 2u;
  } else if (f->tx_len == 2u && f->tx[1] == 0x20u &&
             ((f->tx[0] == 0x93u && f->card == READY1) ||
              (f->tx[0] == 0x95u && f->card == READY2))) {
    const uint8_t cl1[5] = {0x88u, 4u, 1u, 2u, 0x8fu};
    const uint8_t cl2[5] = {3u, 4u, 5u, 6u, 4u};
    memcpy(f->rx, f->tx[0] == 0x93u ? cl1 : cl2, 5u);
    f->rx_len = 5u;
  } else if (f->tx_len == 7u && f->tx[1] == 0x70u &&
             ((f->tx[0] == 0x93u && f->card == READY1) ||
              (f->tx[0] == 0x95u && f->card == READY2))) {
    assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
    f->rx[0] = f->tx[0] == 0x93u ? 4u : 0u;
    f->rx_len = 1u;
    if (f->sak_crc_bytes) {
      f->rx[1] = f->rx[0] == 4u ? 0xdau : 0xfeu;
      f->rx[2] = f->rx[0] == 4u ? 0x17u : 0x51u;
      f->rx_len = 3u;
      if (f->corrupt_sak_crc) f->rx[2] ^= 1u;
    }
    if (f->short_sak_bits) {
      f->rx[0] = 0x0au;
      f->rx_last_bits = (uint8_t)f->short_sak_bits;
    }
    f->card = f->tx[0] == 0x93u ? READY2 : ACTIVE;
  } else if (f->tx_len == 2u && f->tx[0] == 0x30u && f->card == ACTIVE) {
    assert(f->registers[H2_FM175XX_REG_TX_MODE] & 0x80u);
    const size_t offset = (size_t)f->tx[1] * 4u;
    assert(offset + 16u <= sizeof(f->memory));
    memcpy(f->rx, f->memory + offset, 16u);
    f->rx_len = 16u;
    ++f->reads;
    if (f->writes != 0u) ++f->reads_after_writes;
    bool inject_error = f->writes >= f->read_error_after_writes;
    if (inject_error && f->partial_read_bits != 0u &&
        f->tx[1] == f->read_error_page) {
      f->rx_len = (f->partial_read_bits + 7u) / 8u;
      f->rx_last_bits = f->partial_read_bits & 7u;
      f->registers[H2_FM175XX_REG_ERROR] = f->read_error;
      f->registers[H2_FM175XX_REG_COMIRQ] = f->read_error != 0u ? 0x22u : 0x20u;
      return;
    } else if (inject_error && f->read_error != 0u &&
               f->tx[1] == f->read_error_page) {
      f->registers[H2_FM175XX_REG_ERROR] = f->read_error;
      f->rx_len = 0u;
      f->registers[H2_FM175XX_REG_COMIRQ] = 0x22u;
      return;
    } else if (f->last_readable_page != 0u &&
               f->tx[1] > f->last_readable_page) {
      f->rx[0] = 0u;
      f->rx_len = 1u;
      f->rx_last_bits = 4u;
      ++f->rejected_reads;
    } else if (f->read_crc_bytes) {
      uint16_t crc = reply_crc(f->rx, 16u);
      f->rx[16] = (uint8_t)crc;
      f->rx[17] = (uint8_t)(crc >> 8u);
      f->rx_len = 18u;
      if (inject_error && f->corrupt_read_crc) f->rx[17] ^= 1u;
    }
  } else if (f->tx_len == 6u && f->tx[0] == 0xa2u && f->card == ACTIVE) {
    const size_t offset = (size_t)f->tx[1] * 4u;
    assert(offset >= 16u && offset + 4u <= sizeof(f->memory));
    ++f->writes;
    bool accepted = f->writes != f->reject_write;
    if (accepted && !f->discard_all_writes && f->writes != f->discard_write)
      memcpy(f->memory + offset, f->tx + 2u, 4u);
    f->rx[0] = accepted ? 0x0au : 0u;
    f->rx_len = 1u;
    f->rx_last_bits = 4u;
    if (f->write_reply_bits != 0u) {
      f->rx[0] = f->write_reply_low4;
      memcpy(f->rx + 1u, f->write_reply_tail, sizeof(f->write_reply_tail));
      f->rx_len = (f->write_reply_bits + 7u) / 8u;
      f->rx_last_bits = f->write_reply_bits & 7u;
    }
    if (f->write_timeout || f->write_error != 0u) {
      f->rx_len = 0u;
      f->registers[H2_FM175XX_REG_ERROR] = f->write_error;
      f->registers[H2_FM175XX_REG_COMIRQ] = f->write_timeout ? 1u : 0x22u;
      return;
    }
    if (f->write_reply_crc) {
      uint16_t crc = reply_crc(f->rx, 1u);
      f->rx[1] = (uint8_t)crc;
      f->rx[2] = (uint8_t)(crc >> 8u);
      if (f->corrupt_write_reply_crc) f->rx[2] ^= 1u;
      f->rx_len = 3u;
      f->rx_last_bits = 0u;
    }
  } else {
    return;
  }
  if ((f->empty_before_reply || f->reply_delay_us != 0u) && f->rx_len != 0u) {
    memcpy(f->pending_rx, f->rx, f->rx_len);
    f->pending_rx_len = f->rx_len;
    f->pending_last_bits = f->rx_last_bits;
    f->rx_len = 0u;
    f->registers[H2_FM175XX_REG_COMIRQ] = 0x20u;
  } else {
    f->registers[H2_FM175XX_REG_COMIRQ] =
        f->response_irq ? f->response_irq : 0x20u;
  }
}

static int write_register(void* user, uint8_t reg, uint8_t value) {
  fixture_t* f = user;
  if (f->fail_register) return H2_FM175XX_ERR_IO;
  if (reg == H2_FM175XX_REG_COMMAND && value == H2_FM175XX_CMD_IDLE &&
      f->fail_post_idle &&
      f->registers[H2_FM175XX_REG_COMMAND] == H2_FM175XX_CMD_TRANSCEIVE) {
    f->fail_post_idle = 0;
    return H2_FM175XX_ERR_IO;
  }
  if (reg == H2_FM175XX_REG_COMIRQ) {
    f->registers[reg] &= (uint8_t)~value;
  } else if (reg == H2_FM175XX_REG_FIFO_LEVEL && (value & 0x80u)) {
    f->tx_len = 0u;
    f->rx_len = 0u;
    f->pending_rx_len = 0u;
    f->registers[reg] = 0u;
  } else if (reg == H2_FM175XX_REG_TX_CTRL && f->reject_rf_enable) {
    f->registers[reg] = value & 0xfcu;
  } else if (reg == H2_FM175XX_REG_RX_SEL && f->reject_receiver_select) {
    f->registers[reg] = 0x04u;
  } else if (reg == H2_FM175XX_REG_RFCFG && f->reject_rf_gain) {
    /* A transport ACK is insufficient when the selected gain did not stick. */
  } else if (reg == H2_FM175XX_REG_CONTROL) {
    /* RxLastBits remains zero for these byte-aligned replies. */
    f->registers[reg] = value & 0xf8u;
  } else {
    f->registers[reg] = value;
  }
  if (reg == H2_FM175XX_REG_COMMAND && value == H2_FM175XX_CMD_IDLE &&
      f->clear_error_on_idle)
    f->registers[H2_FM175XX_REG_ERROR] = 0u;
  if (reg == H2_FM175XX_REG_COMMAND && value == H2_FM175XX_CMD_SOFT_RESET) {
    ++f->soft_resets;
    f->registers[H2_FM175XX_REG_GSN] = f->reset_gsn;
    f->registers[H2_FM175XX_REG_GWGSP] = f->reset_gwgsp;
  }
  if (reg == H2_FM175XX_REG_COMMAND && value == H2_FM175XX_CMD_TRANSCEIVE) {
    f->registers[H2_FM175XX_REG_COMIRQ] = f->no_initial_tx_alert ? 0u : 4u;
    f->frame_elapsed_us = 0u;
  }
  if (reg == H2_FM175XX_REG_BIT_FRAMING && (value & 0x80u) &&
      f->registers[H2_FM175XX_REG_COMMAND] == H2_FM175XX_CMD_TRANSCEIVE) {
    exchange(f);
    f->tx_len = 0u;
  }
  return H2_FM175XX_OK;
}

static int write_registers(void* user, uint8_t reg, const uint8_t* data,
                           size_t len) {
  fixture_t* f = user;
  assert(reg == H2_FM175XX_REG_FIFO_DATA && len <= sizeof(f->tx));
  memcpy(f->tx, data, len);
  f->tx_len = f->truncate_fifo_write && len > 1u ? 1u : len;
  return H2_FM175XX_OK;
}

static void deliver_pending(fixture_t* f) {
  assert(f->pending_rx_len != 0u);
  memcpy(f->rx, f->pending_rx, f->pending_rx_len);
  f->rx_len = f->pending_rx_len;
  f->rx_last_bits = f->pending_last_bits;
  f->pending_rx_len = 0u;
  ++f->delayed_replies;
  f->registers[H2_FM175XX_REG_COMIRQ] =
      f->response_irq ? f->response_irq : 0x20u;
}

static int read_register(void* user, uint8_t reg, uint8_t* out) {
  fixture_t* f = user;
  if (f->fail_register) return H2_FM175XX_ERR_IO;
  *out = reg == H2_FM175XX_REG_FIFO_LEVEL ? (uint8_t)(f->rx_len + f->tx_len)
                                          : f->registers[reg];
  if (reg == H2_FM175XX_REG_CONTROL)
    *out =
        (uint8_t)((*out & 0xf8u) | (f->rx_len == 0u && f->pending_rx_len != 0u
                                        ? f->empty_last_bits
                                        : f->rx_last_bits));
  if (reg == H2_FM175XX_REG_FIFO_LEVEL && *out == 0u &&
      f->reply_on_empty_probe && f->pending_rx_len != 0u &&
      f->registers[H2_FM175XX_REG_COMMAND] == H2_FM175XX_CMD_TRANSCEIVE) {
    /* A real packet completes after the empty FIFO sample but before the
     * caller's next register access. It raises a new RX completion. */
    deliver_pending(f);
  }
  return H2_FM175XX_OK;
}

static int read_registers(void* user, uint8_t reg, uint8_t* out, size_t len) {
  fixture_t* f = user;
  assert(reg == H2_FM175XX_REG_FIFO_DATA && len == f->rx_len);
  memcpy(out, f->rx, len);
  f->rx_len = 0u;
  return H2_FM175XX_OK;
}

static void sleep_ms(void* user, uint32_t ms) {
  fixture_t* f = user;
  if (f->registers[H2_FM175XX_REG_COMMAND] != H2_FM175XX_CMD_TRANSCEIVE ||
      (f->registers[H2_FM175XX_REG_COMIRQ] & 0x20u) != 0u)
    return;
  f->frame_elapsed_us += ms * 1000u;
  if (f->pending_rx_len != 0u) {
    const uint32_t prescaler =
        ((uint32_t)(f->registers[H2_FM175XX_REG_TMODE] & 0x0fu) << 8u) |
        f->registers[H2_FM175XX_REG_TPRESCALER];
    const uint32_t reload =
        ((uint32_t)f->registers[H2_FM175XX_REG_TRELOAD_HI] << 8u) |
        f->registers[H2_FM175XX_REG_TRELOAD_LO];
    const uint32_t budget_us =
        (uint32_t)((uint64_t)reload * (2u * prescaler + 1u) * 1000u / 13560u);
    const uint32_t delay_us = f->reply_delay_us ? f->reply_delay_us : 200u;
    if (delay_us > budget_us) {
      if (f->frame_elapsed_us >= budget_us)
        f->registers[H2_FM175XX_REG_COMIRQ] |= 0x01u;
      return;
    }
    if (f->frame_elapsed_us < delay_us) return;
    deliver_pending(f);
  } else if (f->rx_len == 0u) {
    f->registers[H2_FM175XX_REG_COMIRQ] |= 0x01u;
  }
}

static void prepare_ndef_reader(fixture_t* fixture, h2_fm175xx_t* reader) {
  *fixture = (fixture_t){.card = IDLE};
  fixture->memory[12] = 0xe1u;
  fixture->memory[13] = 0x10u;
  fixture->memory[14] = 18u;
  fixture->memory[16] = 3u;
  fixture->memory[17] = 6u;
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
  memcpy(fixture->memory + 18u, message, sizeof(message));
  fixture->memory[24] = 0xfeu;
  const h2_fm175xx_transport_t transport = {
      .user = fixture,
      .write_reg = write_register,
      .write_regs = write_registers,
      .read_reg = read_register,
      .read_regs = read_registers,
      .sleep_ms = sleep_ms,
  };
  assert(h2_fm175xx_init(reader, &transport) == H2_FM175XX_OK);
  assert(h2_fm175xx_open_type_a(reader) == H2_FM175XX_OK);
  h2_fm175xx_type_a_card_t card;
  assert(h2_fm175xx_type_a_activate(reader, &card) == H2_FM175XX_OK);
}

static void test_read_retained_crc(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_ndef_reader(&fixture, &reader);
  fixture.read_crc_bytes = 1;
  uint8_t pages[sizeof(fixture.memory)];
  size_t length = 0u;
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &length) ==
         H2_FM175XX_OK);
  assert(length == 160u);
  assert(memcmp(pages, fixture.memory, length) == 0);
  fixture.corrupt_read_crc = 1;
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &length) ==
         H2_FM175XX_ERR_PROTOCOL);
}

static void test_ndef_public_region(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_ndef_reader(&fixture, &reader);
  fixture.last_readable_page = 4u;
  uint8_t message[32];
  size_t length = 0u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, message, sizeof(message),
                                   &length) == H2_FM175XX_OK);
  assert(length == 6u && memcmp(message, fixture.memory + 18u, length) == 0);
  assert(fixture.reads == 2u && fixture.rejected_reads == 0u);
  assert(fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.last_readable_page = 4u;
  fixture.read_crc_bytes = 1;
  h2_fm175xx_read_diagnostics_t diagnostics;
  assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, message, sizeof(message),
                                              &length,
                                              &diagnostics) == H2_FM175XX_OK);
  assert(length == 6u && fixture.reads == 2u);
  assert(diagnostics.read_count == 2u &&
         diagnostics.retained_crc_frames == 2u &&
         diagnostics.last_reply_bits == 144u && diagnostics.last_page == 4u &&
         diagnostics.last_result == H2_FM175XX_OK);
  fixture.corrupt_read_crc = 1;
  assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, message, sizeof(message),
                                              &length, &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(length == 0u && diagnostics.read_count == 1u &&
         diagnostics.last_result == H2_FM175XX_ERR_PROTOCOL);

  prepare_ndef_reader(&fixture, &reader);
  fixture.last_readable_page = 4u;
  memset(message, 0xa5, sizeof(message));
  assert(h2_fm175xx_ntag_read_ndef(&reader, message, 5u, &length) ==
         H2_FM175XX_ERR_NO_MEMORY);
  assert(length == 0u && message[0] == 0xa5u);
  assert(fixture.reads == 2u && fixture.rejected_reads == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.memory[17] = 145u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, message, sizeof(message),
                                   &length) == H2_FM175XX_ERR_PROTOCOL);
  assert(length == 0u && fixture.reads == 2u);

  prepare_ndef_reader(&fixture, &reader);
  const uint8_t expected[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
  memset(fixture.memory + 16u, 0, sizeof(fixture.memory) - 16u);
  fixture.memory[16] = 0xfdu;
  fixture.memory[17] = 13u;
  fixture.memory[31] = 3u;
  fixture.memory[32] = sizeof(expected);
  memcpy(fixture.memory + 33u, expected, sizeof(expected));
  fixture.memory[39] = 0xfeu;
  fixture.last_readable_page = 8u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, message, sizeof(message),
                                   &length) == H2_FM175XX_OK);
  assert(length == sizeof(expected) &&
         memcmp(message, expected, sizeof(expected)) == 0);
  assert(fixture.reads == 3u && fixture.rejected_reads == 0u);
  fixture.memory[32] = 255u;
  fixture.memory[33] = 1u;
  fixture.memory[34] = 0u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, message, sizeof(message),
                                   &length) == H2_FM175XX_ERR_PROTOCOL);
  assert(length == 0u && fixture.rejected_reads == 0u);
}

static void test_ndef_receive_capacity(void) {
  const size_t lengths[] = {300u, 512u, 513u};
  for (size_t index = 0u; index < sizeof(lengths) / sizeof(lengths[0]);
       ++index) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_ndef_reader(&fixture, &reader);
    fixture.memory[14] = 126u;
    size_t message_length = lengths[index];
    size_t payload_length = message_length - 7u;
    uint8_t expected[513];
    memset(expected, 'x', sizeof(expected));
    expected[0] = 0xc1u;
    expected[1] = 1u;
    expected[2] = 0u;
    expected[3] = 0u;
    expected[4] = (uint8_t)(payload_length >> 8u);
    expected[5] = (uint8_t)payload_length;
    expected[6] = 'U';
    expected[7] = 4u;
    fixture.memory[17] = 255u;
    fixture.memory[18] = (uint8_t)(message_length >> 8u);
    fixture.memory[19] = (uint8_t)message_length;
    memcpy(fixture.memory + 20u, expected, message_length);
    fixture.memory[20u + message_length] = 0xfeu;
    uint8_t output[512];
    memset(output, 0xa5, sizeof(output));
    size_t length = 0u;
    h2_fm175xx_read_diagnostics_t diagnostics;
    assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, output, 256u, &length,
                                                &diagnostics) ==
           H2_FM175XX_ERR_NO_MEMORY);
    assert(length == 0u && diagnostics.required_capacity == message_length &&
           diagnostics.read_count == 2u && diagnostics.last_result == 0);
    int result = h2_fm175xx_ntag_read_ndef_diagnostic(
        &reader, output, sizeof(output), &length, &diagnostics);
    assert(diagnostics.required_capacity == message_length);
    if (message_length > sizeof(output)) {
      assert(result == H2_FM175XX_ERR_NO_MEMORY && length == 0u &&
             diagnostics.read_count == 2u);
      for (size_t byte = 0u; byte < sizeof(output); ++byte)
        assert(output[byte] == 0xa5u);
    } else {
      assert(result == H2_FM175XX_OK && length == message_length &&
             memcmp(output, expected, message_length) == 0);
      size_t reads = (20u + message_length + 15u) / 16u;
      assert(diagnostics.read_count == reads &&
             diagnostics.last_page == (reads - 1u) * 4u);
    }
    assert(fixture.writes == 0u);
  }
}

static void test_read_error_snapshot(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_ndef_reader(&fixture, &reader);
  fixture.read_error = 4u;
  fixture.read_error_page = 4u;
  uint8_t output[32];
  memset(output, 0xa5, sizeof(output));
  size_t length = 0u;
  h2_fm175xx_read_diagnostics_t diagnostics;
  assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, output, sizeof(output),
                                              &length, &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(length == 0u && diagnostics.read_count == 2u &&
         diagnostics.last_page == 4u && diagnostics.last_reply_bits == 0u &&
         diagnostics.last_result == H2_FM175XX_ERR_PROTOCOL &&
         diagnostics.status_result == H2_FM175XX_OK &&
         diagnostics.last_error == 4u && diagnostics.last_fifo == 0u);
  assert(fixture.writes == 0u);
  for (size_t index = 0u; index < sizeof(output); ++index)
    assert(output[index] == 0xa5u);
}

static void test_read_partial_error_snapshot(void) {
  for (unsigned scenario = 0u; scenario < 2u; ++scenario) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_ndef_reader(&fixture, &reader);
    fixture.partial_read_bits = 96u;
    fixture.read_error_page = 4u;
    fixture.read_error = scenario == 0u ? 0u : 4u;
    fixture.clear_error_on_idle = true;
    uint8_t output[32];
    memset(output, 0xa5, sizeof(output));
    size_t length = 0u;
    h2_fm175xx_read_diagnostics_t diagnostics;
    assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, output, sizeof(output),
                                                &length, &diagnostics) ==
           H2_FM175XX_ERR_PROTOCOL);
    assert(length == 0u && diagnostics.last_reply_bits == 96u &&
           diagnostics.last_page == 4u && diagnostics.read_count == 2u &&
           diagnostics.last_result == H2_FM175XX_ERR_PROTOCOL);
    assert(diagnostics.rx_status_result == H2_FM175XX_OK &&
           diagnostics.rx_irq == (scenario == 0u ? 0x20u : 0x22u) &&
           diagnostics.rx_error == fixture.read_error &&
           diagnostics.rx_fifo == 12u &&
           diagnostics.status_result == H2_FM175XX_OK &&
           diagnostics.last_error == 0u);
    assert(fixture.writes == 0u);
    for (size_t index = 0u; index < sizeof(output); ++index)
      assert(output[index] == 0xa5u);
  }
}

static void test_activation_diagnostics(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  h2_fm175xx_type_a_card_t card;
  h2_fm175xx_activation_diagnostics_t diagnostics;

  prepare_ndef_reader(&fixture, &reader);
  fixture.unexpected_halt_reply = 1;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_HALT &&
         diagnostics.command == 0x50u && diagnostics.reply_bits == 8u &&
         diagnostics.cascade_level == 0u &&
         diagnostics.status_result == H2_FM175XX_OK);
  assert(card.uid_len == 0u && fixture.reads == 0u && fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.short_atqa = 1;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_REQUEST &&
         diagnostics.command == 0x52u && diagnostics.reply_bits == 8u &&
         diagnostics.status_result == H2_FM175XX_OK);
  assert(card.uid_len == 0u && fixture.reads == 0u && fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.short_sak_bits = 4;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_SELECT &&
         diagnostics.command == 0x93u && diagnostics.reply_bits == 4u &&
         diagnostics.cascade_level == 1u &&
         diagnostics.status_result == H2_FM175XX_OK);
  assert(card.uid_len == 0u && fixture.reads == 0u && fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.card = ABSENT;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_ERR_TIMEOUT);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_REQUEST &&
         diagnostics.command == 0x52u && diagnostics.reply_bits == 0u &&
         diagnostics.status_result == H2_FM175XX_OK);
  assert(card.uid_len == 0u && fixture.reads == 0u && fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.fail_register = 1;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_ERR_IO);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_FIELD &&
         diagnostics.command == 0u && diagnostics.reply_bits == 0u &&
         diagnostics.status_result == H2_FM175XX_ERR_IO);
  assert(card.uid_len == 0u && fixture.reads == 0u && fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  fixture.sak_crc_bytes = 1;
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, &diagnostics) ==
         H2_FM175XX_OK);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_COMPLETE &&
         diagnostics.command == 0x95u && diagnostics.reply_bits == 24u &&
         diagnostics.cascade_level == 2u &&
         diagnostics.status_result == H2_FM175XX_ERR_UNAVAILABLE);
  assert(card.uid_len == 10u && fixture.reads == 0u && fixture.writes == 0u);
  assert(h2_fm175xx_type_a_activate_diagnostic(&reader, &card, NULL) ==
         H2_FM175XX_OK);
  assert(h2_fm175xx_type_a_activate_diagnostic(NULL, &card, &diagnostics) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(diagnostics.phase == H2_FM175XX_ACTIVATION_FIELD &&
         diagnostics.command == 0u && diagnostics.reply_bits == 0u &&
         diagnostics.status_result == H2_FM175XX_ERR_UNAVAILABLE);
}

static void test_write_diagnostics(void) {
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'y'};
  const struct {
    size_t offset;
    uint8_t value;
    h2_fm175xx_write_phase_t phase;
  } denied[] = {
      {12u, 0u, H2_FM175XX_WRITE_CC},
      {13u, 0x20u, H2_FM175XX_WRITE_CC},
      {15u, 0x0fu, H2_FM175XX_WRITE_CC},
      {10u, 1u, H2_FM175XX_WRITE_LOCKS},
      {11u, 1u, H2_FM175XX_WRITE_LOCKS},
      {16u, 0xfdu, H2_FM175XX_WRITE_LAYOUT},
      {24u, 0xfdu, H2_FM175XX_WRITE_TRAILING_TLV},
  };
  for (size_t index = 0u; index < sizeof(denied) / sizeof(denied[0]); ++index) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_ndef_reader(&fixture, &reader);
    fixture.memory[denied[index].offset] = denied[index].value;
    uint8_t original[sizeof(fixture.memory)];
    memcpy(original, fixture.memory, sizeof(original));
    h2_fm175xx_write_diagnostics_t diagnostics;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) ==
           H2_FM175XX_ERR_UNAVAILABLE);
    assert(diagnostics.phase == denied[index].phase &&
           diagnostics.metadata_valid == 1u &&
           diagnostics.tag_capacity == 160u && diagnostics.write_calls == 0u &&
           fixture.writes == 0u);
    assert(memcmp(diagnostics.cc, fixture.memory + 12u, 4u) == 0);
    assert(memcmp(diagnostics.static_locks, fixture.memory + 10u, 2u) == 0);
    assert(memcmp(original, fixture.memory, sizeof(original)) == 0);
  }

  fixture_t fixture;
  h2_fm175xx_t reader;
  h2_fm175xx_write_diagnostics_t diagnostics;
  prepare_ndef_reader(&fixture, &reader);
  fixture.memory[14] = 255u;
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(&reader, message,
                                               sizeof(message), &diagnostics) ==
         H2_FM175XX_ERR_UNAVAILABLE);
  assert(diagnostics.phase == H2_FM175XX_WRITE_READ_TAG &&
         diagnostics.metadata_valid == 0u && diagnostics.write_calls == 0u &&
         fixture.writes == 0u);

  prepare_ndef_reader(&fixture, &reader);
  uint8_t oversized[150] = {0};
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, oversized, sizeof(oversized), &diagnostics) ==
         H2_FM175XX_ERR_NO_MEMORY);
  assert(diagnostics.phase == H2_FM175XX_WRITE_CAPACITY &&
         diagnostics.write_calls == 0u && fixture.writes == 0u);

  const h2_fm175xx_write_phase_t rejected[] = {
      H2_FM175XX_WRITE_INVALIDATE, H2_FM175XX_WRITE_PAYLOAD,
      H2_FM175XX_WRITE_PAYLOAD, H2_FM175XX_WRITE_PUBLISH};
  for (size_t index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
       ++index) {
    prepare_ndef_reader(&fixture, &reader);
    fixture.reject_write = (unsigned)(index + 1u);
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) ==
           H2_FM175XX_ERR_PROTOCOL);
    assert(diagnostics.phase == rejected[index] &&
           diagnostics.write_calls == index + 1u &&
           fixture.writes == index + 1u);
    assert(diagnostics.last_reply_bits == 4u &&
           diagnostics.last_reply_low4 == 0u &&
           diagnostics.last_command_result == H2_FM175XX_OK &&
           diagnostics.last_restore_result == H2_FM175XX_OK &&
           diagnostics.status_result == H2_FM175XX_OK &&
           diagnostics.last_error == 0u);
  }

  prepare_ndef_reader(&fixture, &reader);
  fixture.discard_write = 4u;
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, message, sizeof(message), &diagnostics) == H2_FM175XX_OK);
  assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
         diagnostics.write_calls == 4u && fixture.writes == 4u &&
         fixture.memory[17] == 0u && fixture.reads_after_writes == 0u);
  uint8_t output[32];
  size_t length = 0u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, output, sizeof(output), &length) ==
         H2_FM175XX_ERR_UNAVAILABLE);

  prepare_ndef_reader(&fixture, &reader);
  fixture.discard_write = 2u;
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, message, sizeof(message), &diagnostics) == H2_FM175XX_OK);
  assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
         diagnostics.write_calls == 4u && fixture.writes == 4u &&
         fixture.reads_after_writes == 0u);
  assert(h2_fm175xx_ntag_read_ndef(&reader, output, sizeof(output), &length) ==
         H2_FM175XX_OK);
  assert(length == sizeof(message) && memcmp(output, message, length) != 0);

  prepare_ndef_reader(&fixture, &reader);
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, message, sizeof(message), &diagnostics) == H2_FM175XX_OK);
  assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
         diagnostics.write_calls == 4u && fixture.writes == 4u &&
         diagnostics.last_page == 4u && diagnostics.last_reply_bits == 4u &&
         diagnostics.last_reply_low4 == 10u &&
         diagnostics.status_result == H2_FM175XX_ERR_UNAVAILABLE &&
         fixture.reads_after_writes == 0u);
  assert(diagnostics.before_ndef_valid && diagnostics.expected_ndef_valid);
  char before_hex[65], expected_hex[65];
  h2_sha256_hex(diagnostics.before_ndef_sha256, before_hex);
  h2_sha256_hex(diagnostics.expected_ndef_sha256, expected_hex);
  assert(
      strcmp(
          before_hex,
          "7324d2ae1f5e9bde29094b8180be5892e9ef61e99993b1b138c05806641da77b") ==
      0);
  assert(
      strcmp(
          expected_hex,
          "6c05e5971942727214687e986a1360f3344507eb878f39bb9aa27a4952e053c8") ==
      0);
  assert(h2_fm175xx_ntag_read_ndef(&reader, output, sizeof(output), &length) ==
         H2_FM175XX_OK);
  assert(length == sizeof(message) && memcmp(output, message, length) == 0);
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(NULL, message, sizeof(message),
                                               &diagnostics) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(diagnostics.phase == H2_FM175XX_WRITE_ARGUMENT &&
         diagnostics.metadata_valid == 0u && diagnostics.write_calls == 0u);
}

static void test_write_does_not_read_back(void) {
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'y'};
  const size_t expected_bits[] = {0u, 96u, 144u};
  for (unsigned scenario = 0u; scenario < 3u; ++scenario) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_ndef_reader(&fixture, &reader);
    fixture.read_error_after_writes = 4u;
    fixture.read_error_page = 4u;
    fixture.read_error = scenario == 0u ? 8u : 0u;
    fixture.partial_read_bits = scenario == 1u ? 96u : 0u;
    fixture.read_crc_bytes = scenario == 2u;
    fixture.corrupt_read_crc = scenario == 2u;
    h2_fm175xx_write_diagnostics_t diagnostics;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) ==
           H2_FM175XX_OK);
    assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
           diagnostics.write_calls == 4u && fixture.writes == 4u &&
           fixture.reads_after_writes == 0u);
    uint8_t output[32];
    size_t length = 0u;
    h2_fm175xx_read_diagnostics_t read_diagnostics;
    assert(h2_fm175xx_ntag_read_ndef_diagnostic(&reader, output, sizeof(output),
                                                &length, &read_diagnostics) ==
           H2_FM175XX_ERR_PROTOCOL);
    assert(length == 0u &&
           read_diagnostics.last_reply_bits == expected_bits[scenario] &&
           read_diagnostics.last_result == H2_FM175XX_ERR_PROTOCOL &&
           read_diagnostics.rx_status_result == H2_FM175XX_OK &&
           read_diagnostics.rx_error == fixture.read_error &&
           read_diagnostics.read_count == (scenario == 2u ? 1u : 2u));
  }
}

static void test_write_reply_diagnostics(void) {
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'y'};
  for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    h2_fm175xx_write_diagnostics_t diagnostics;
    prepare_ndef_reader(&fixture, &reader);
    fixture.write_reply_bits = scenario == 0u ? 3u : 8u;
    fixture.write_reply_low4 = 10u;
    fixture.write_timeout = scenario == 2u;
    fixture.write_error = scenario == 3u ? 8u : 0u;
    fixture.clear_error_on_idle = scenario == 3u;
    int expected =
        scenario == 2u ? H2_FM175XX_ERR_TIMEOUT : H2_FM175XX_ERR_PROTOCOL;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) == expected);
    assert(diagnostics.phase == H2_FM175XX_WRITE_INVALIDATE &&
           diagnostics.write_calls == 1u && fixture.writes == 1u &&
           diagnostics.last_reply_bits ==
               (scenario >= 2u ? 0u : fixture.write_reply_bits) &&
           diagnostics.last_reply_low4 == 0u &&
           diagnostics.last_command_result ==
               (scenario >= 2u ? expected : H2_FM175XX_OK) &&
           diagnostics.last_restore_result == H2_FM175XX_OK &&
           diagnostics.status_result == H2_FM175XX_OK &&
           diagnostics.last_error == 0u);
    assert(diagnostics.rx_status_result ==
               (scenario == 2u ? H2_FM175XX_ERR_UNAVAILABLE : H2_FM175XX_OK) &&
           diagnostics.rx_error == fixture.write_error);
  }
}

static void test_write_partial_reply_snapshot(void) {
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'y'};
  const uint8_t lengths[] = {10u, 16u, 18u};
  for (size_t index = 0u; index < sizeof(lengths); ++index) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    h2_fm175xx_write_diagnostics_t diagnostics;
    prepare_ndef_reader(&fixture, &reader);
    fixture.write_reply_bits = lengths[index];
    fixture.write_reply_low4 = 0x0au;
    fixture.write_reply_tail[0] = 0xa4u;
    fixture.write_reply_tail[1] = 0xfeu;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) ==
           H2_FM175XX_ERR_PROTOCOL);
    assert(fixture.writes == 1u && diagnostics.write_calls == 1u &&
           diagnostics.last_reply_bits == lengths[index] &&
           diagnostics.last_reply_low4 == 0u &&
           diagnostics.last_reply[0] == 0x0au &&
           diagnostics.last_reply[1] == 0xa4u &&
           diagnostics.last_reply[2] == (lengths[index] > 16u ? 0xfeu : 0u));
    assert(diagnostics.rx_status_result == H2_FM175XX_OK &&
           diagnostics.rx_irq == 0x20u && diagnostics.rx_error == 0u &&
           diagnostics.rx_fifo == (lengths[index] + 7u) / 8u &&
           (diagnostics.rx_control & 7u) == (lengths[index] & 7u));
    assert(diagnostics.configuration_result == H2_FM175XX_OK &&
           diagnostics.last_tx_mode == 0x80u &&
           diagnostics.last_rx_mode == 0u && diagnostics.last_framing == 0u &&
           fixture.registers[H2_FM175XX_REG_RX_MODE] == 0x80u);
  }
}

static void test_write_reply_crc(void) {
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'y'};
  for (unsigned scenario = 0u; scenario < 4u; ++scenario) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    h2_fm175xx_write_diagnostics_t diagnostics;
    prepare_ndef_reader(&fixture, &reader);
    fixture.write_reply_crc = true;
    fixture.corrupt_write_reply_crc = scenario == 1u;
    fixture.reject_write = scenario == 2u ? 1u : 0u;
    if (scenario == 3u) {
      fixture.write_reply_bits = 8u;
      fixture.write_reply_low4 = 0xaau;
    }
    int expected = scenario == 0u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) == expected);
    assert(diagnostics.last_reply_bits == 24u &&
           fixture.writes == (scenario == 0u ? 4u : 1u) &&
           diagnostics.last_reply_crc_valid == (scenario != 1u) &&
           diagnostics.last_reply_low4 == (scenario == 0u ? 10u : 0u));
    if (scenario == 0u) {
      const uint8_t captured_ack[] = {0x0au, 0xa4u, 0xfeu};
      assert(memcmp(diagnostics.last_reply, captured_ack,
                    sizeof(captured_ack)) == 0);
      assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE);
    } else {
      assert(diagnostics.phase == H2_FM175XX_WRITE_INVALIDATE &&
             diagnostics.write_calls == 1u);
    }
  }
}

static void prepare_control_ndef(fixture_t* fixture, h2_fm175xx_t* reader,
                                 size_t ndef_offset) {
  prepare_ndef_reader(fixture, reader);
  memset(fixture->memory + 16u, 0, sizeof(fixture->memory) - 16u);
  fixture->memory[14] = 0x33u;
  const uint8_t control[] = {1u, 3u, 0xd8u, 45u, 0x35u};
  memcpy(fixture->memory + 16u, control, sizeof(control));
  fixture->memory[ndef_offset] = 3u;
  fixture->memory[ndef_offset + 1u] = 255u;
  fixture->memory[ndef_offset + 2u] = 1u;
  fixture->memory[ndef_offset + 3u] = 36u;
  memset(fixture->memory + ndef_offset + 4u, 'x', 292u);
  fixture->memory[ndef_offset + 296u] = 0xfeu;
}

static void test_write_control_prefix(void) {
  const size_t offsets[] = {21u, 22u, 23u, 24u, 25u, 26u, 27u, 28u};
  const size_t lengths[] = {1u, 46u, 254u, 255u, 292u, 370u};
  for (size_t offset_index = 0u;
       offset_index < sizeof(offsets) / sizeof(offsets[0]); ++offset_index) {
    for (size_t length_index = 0u;
         length_index < sizeof(lengths) / sizeof(lengths[0]); ++length_index) {
      fixture_t fixture;
      h2_fm175xx_t reader;
      size_t offset = offsets[offset_index], length = lengths[length_index];
      prepare_control_ndef(&fixture, &reader, offset);
      uint8_t original[sizeof(fixture.memory)];
      memcpy(original, fixture.memory, sizeof(original));
      uint8_t message[370], output[370];
      memset(message, 'y', sizeof(message));
      h2_fm175xx_write_diagnostics_t diagnostics;
      assert(h2_fm175xx_ntag_write_ndef_diagnostic(
                 &reader, message, length, &diagnostics) == H2_FM175XX_OK);
      assert(diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
             diagnostics.write_calls == fixture.writes);
      assert(diagnostics.ndef_offset == offset &&
             diagnostics.control_tlvs == 1u);
      assert(memcmp(diagnostics.control_tlv, original + 16u, 5u) == 0);
      assert(memcmp(original, fixture.memory, offset) == 0);
      size_t end = offset + (length < 255u ? 2u : 4u) + length + 1u;
      assert(memcmp(original + end, fixture.memory + end,
                    sizeof(original) - end) == 0);
      size_t received = 0u;
      assert(h2_fm175xx_ntag_read_ndef(&reader, output, sizeof(output),
                                       &received) == H2_FM175XX_OK);
      assert(received == length && memcmp(output, message, length) == 0);
    }
  }

  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_control_ndef(&fixture, &reader, 26u);
  const uint8_t reserved[] = {2u, 3u, 0xc8u, 8u, 5u};
  memcpy(fixture.memory + 21u, reserved, sizeof(reserved));
  memset(fixture.memory + 392u, 0xa5, 8u);
  uint8_t original[sizeof(fixture.memory)];
  memcpy(original, fixture.memory, sizeof(original));
  uint8_t message[46] = {0xd1u};
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_OK);
  assert(memcmp(original, fixture.memory, 26u) == 0);
  assert(memcmp(original + 392u, fixture.memory + 392u,
                sizeof(original) - 392u) == 0);
}

static void test_acknowledged_unchanged_tag(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_control_ndef(&fixture, &reader, 21u);
  fixture.discard_all_writes = true;
  fixture.write_reply_crc = true;
  uint8_t original[sizeof(fixture.memory)];
  memcpy(original, fixture.memory, sizeof(original));
  uint8_t message[46];
  memset(message, 'y', sizeof(message));
  h2_fm175xx_write_diagnostics_t diagnostics;
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, message, sizeof(message), &diagnostics) == H2_FM175XX_OK);
  assert(fixture.writes == 14u && diagnostics.write_calls == 14u &&
         diagnostics.phase == H2_FM175XX_WRITE_COMPLETE &&
         diagnostics.last_reply_crc_valid == 1u &&
         diagnostics.last_reply_low4 == 10u &&
         fixture.reads_after_writes == 0u);
  assert(diagnostics.before_ndef_valid && diagnostics.expected_ndef_valid &&
         diagnostics.before_ndef_length == 292u);
  char before_hex[65], expected_hex[65];
  h2_sha256_hex(diagnostics.before_ndef_sha256, before_hex);
  h2_sha256_hex(diagnostics.expected_ndef_sha256, expected_hex);
  assert(
      strcmp(
          before_hex,
          "3e13cff4b3bb063095b79191190af13b5015fd954cbcf0c8b0dae4df4de81e2e") ==
      0);
  assert(
      strcmp(
          expected_hex,
          "de17dd5411beb862d9e672f6df01360967ca717a0a65011da36f86d566feffa2") ==
      0);
  assert(strcmp(before_hex, expected_hex) != 0);
  assert(memcmp(fixture.memory, original, sizeof(original)) == 0);
  uint8_t output[512];
  size_t length = 0u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, output, sizeof(output), &length) ==
         H2_FM175XX_OK);
  assert(length == 292u && memcmp(output, original + 25u, length) == 0 &&
         fixture.reads_after_writes == 20u);
}

static void test_write_control_rejections(void) {
  const uint8_t message[46] = {0xd1u};
  const struct {
    size_t offset;
    uint8_t value;
    int result;
  } rejected[] = {
      {17u, 2u, H2_FM175XX_ERR_PROTOCOL},
      {17u, 255u, H2_FM175XX_ERR_PROTOCOL},
      {18u, 0u, H2_FM175XX_ERR_PROTOCOL},
      {20u, 0x3fu, H2_FM175XX_ERR_UNAVAILABLE},
      {18u, 0x01u, H2_FM175XX_ERR_PROTOCOL},
      {424u, 1u, H2_FM175XX_ERR_UNAVAILABLE},
      {429u, 0x10u, H2_FM175XX_ERR_UNAVAILABLE},
  };
  for (size_t index = 0u; index < sizeof(rejected) / sizeof(rejected[0]);
       ++index) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_control_ndef(&fixture, &reader, 21u);
    fixture.memory[rejected[index].offset] = rejected[index].value;
    uint8_t original[sizeof(fixture.memory)];
    memcpy(original, fixture.memory, sizeof(original));
    h2_fm175xx_write_diagnostics_t diagnostics;
    assert(h2_fm175xx_ntag_write_ndef_diagnostic(
               &reader, message, sizeof(message), &diagnostics) ==
           rejected[index].result);
    assert(fixture.writes == 0u && diagnostics.write_calls == 0u);
    assert(memcmp(original, fixture.memory, sizeof(original)) == 0);
  }

  fixture_t fixture;
  h2_fm175xx_t reader;
  prepare_control_ndef(&fixture, &reader, 26u);
  const uint8_t overlap[] = {2u, 3u, 0x60u, 4u, 2u};
  memcpy(fixture.memory + 21u, overlap, sizeof(overlap));
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(fixture.writes == 0u);

  prepare_control_ndef(&fixture, &reader, 26u);
  const uint8_t reserved[] = {2u, 3u, 0xf0u, 4u, 2u};
  memcpy(fixture.memory + 21u, reserved, sizeof(reserved));
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_UNAVAILABLE);
  assert(fixture.writes == 0u);

  prepare_control_ndef(&fixture, &reader, 21u);
  fixture.read_error = 4u;
  fixture.read_error_page = 104u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(fixture.writes == 0u);

  prepare_control_ndef(&fixture, &reader, 21u);
  fixture.memory[21] = 0xfdu;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_UNAVAILABLE);
  assert(fixture.writes == 0u);
}

static void test_write_control_interruption(void) {
  uint8_t message[292];
  memset(message, 'y', sizeof(message));
  for (size_t offset = 21u; offset < 25u; ++offset) {
    fixture_t fixture;
    h2_fm175xx_t reader;
    prepare_control_ndef(&fixture, &reader, offset);
    assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
           H2_FM175XX_OK);
    unsigned calls = fixture.writes;
    for (unsigned rejected = 1u; rejected <= calls; ++rejected) {
      prepare_control_ndef(&fixture, &reader, offset);
      uint8_t original[28];
      memcpy(original, fixture.memory, offset);
      fixture.reject_write = rejected;
      assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
             H2_FM175XX_ERR_PROTOCOL);
      assert(fixture.writes == rejected);
      assert(memcmp(original, fixture.memory, offset) == 0);
      if (rejected > 1u) assert(fixture.memory[offset + 1u] == 0u);
    }
  }
}

static void test_write_preread_diagnostics(void) {
  fixture_t fixture;
  h2_fm175xx_t reader;
  const uint8_t response[] = {0xd1u, 1u, 5u, 'T', 2u, 'e', 'n', 'o', 'k'};
  h2_fm175xx_write_diagnostics_t diagnostics;
  prepare_ndef_reader(&fixture, &reader);
  /* A virtual tag can expose its complete NDEF but reject later pages in
   * the CC-declared memory. Preserve the failed page and four-bit control
   * response while proving that pre-read failure sends no WRITE command. */
  fixture.memory[14] = 62u;
  fixture.memory[17] = 255u;
  fixture.memory[18] = 1u;
  fixture.memory[19] = 36u;
  memset(fixture.memory + 20u, 'x', 292u);
  fixture.memory[312] = 0xfeu;
  fixture.last_readable_page = 76u;
  uint8_t incoming[292];
  size_t length = 0u;
  assert(h2_fm175xx_ntag_read_ndef(&reader, incoming, sizeof(incoming),
                                   &length) == H2_FM175XX_OK);
  assert(length == 292u && fixture.rejected_reads == 0u);
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, response, sizeof(response), &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(diagnostics.phase == H2_FM175XX_WRITE_READ_TAG &&
         diagnostics.write_calls == 0u && fixture.writes == 0u);
  assert(diagnostics.preread.read_count == 21u &&
         diagnostics.preread.last_page == 80u &&
         diagnostics.preread.last_reply_bits == 4u &&
         diagnostics.preread.transfer_result == H2_FM175XX_OK &&
         diagnostics.preread.last_result == H2_FM175XX_ERR_PROTOCOL &&
         diagnostics.preread.short_reply_valid == 1u &&
         diagnostics.preread.short_reply_low4 == 0u &&
         diagnostics.preread.required_capacity == 512u &&
         diagnostics.preread_header_valid == 1u &&
         diagnostics.preread_cc[0] == 0xe1u &&
         diagnostics.preread_cc[2] == 62u && diagnostics.metadata_valid == 0u);
  prepare_ndef_reader(&fixture, &reader);
  fixture.partial_read_bits = 96u;
  fixture.read_error_page = 0u;
  assert(h2_fm175xx_ntag_write_ndef_diagnostic(
             &reader, response, sizeof(response), &diagnostics) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(diagnostics.preread.read_count == 1u &&
         diagnostics.preread.last_page == 0u &&
         diagnostics.preread.last_reply_bits == 96u &&
         diagnostics.preread.short_reply_valid == 0u &&
         diagnostics.preread_header_valid == 0u &&
         diagnostics.write_calls == 0u && fixture.writes == 0u);
}

int main(void) {
  test_write_preread_diagnostics();
  test_write_control_prefix();
  test_write_control_rejections();
  test_write_control_interruption();
  test_write_diagnostics();
  test_write_does_not_read_back();
  test_write_reply_diagnostics();
  test_write_partial_reply_snapshot();
  test_acknowledged_unchanged_tag();
  test_write_reply_crc();
  test_activation_diagnostics();
  test_read_error_snapshot();
  test_read_partial_error_snapshot();
  test_read_retained_crc();
  test_ndef_public_region();
  test_ndef_receive_capacity();
  fixture_t f = {.card = IDLE};
  f.memory[12] = 0xe1u;
  f.memory[13] = 0x10u;
  f.memory[14] = 18u;
  f.memory[16] = 3u;
  f.memory[17] = 1u;
  f.memory[18] = 0xfeu;
  const h2_fm175xx_transport_t transport = {
      .user = &f,
      .write_reg = write_register,
      .write_regs = write_registers,
      .read_reg = read_register,
      .read_regs = read_registers,
      .sleep_ms = sleep_ms,
  };
  h2_fm175xx_t reader;
  assert(h2_fm175xx_init(&reader, &transport) == H2_FM175XX_OK);
  assert(h2_fm175xx_open_type_a(&reader) == H2_FM175XX_OK);
  h2_fm175xx_type_a_card_t first, next;
  assert(h2_fm175xx_type_a_activate(&reader, &first) == H2_FM175XX_OK);
  assert(f.card == ACTIVE);
  /* A Runtime scan has already selected the card. Repeating activation
   * before a content read must not report that same card as absent. */
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  uint8_t pages[256];
  size_t len = 0u;
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &len) ==
         H2_FM175XX_OK);
  assert(len == 160u && memcmp(pages, f.memory, len) == 0);
  assert(f.reads == 10u);
  /* Explicit NDEF reads exclude tag headers. Writes invalidate length first,
   * preserve manufacturer/lock pages, then publish the message. */
  static const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
  uint8_t identity_pages[16];
  memcpy(identity_pages, f.memory, sizeof(identity_pages));
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) == 0);
  assert(memcmp(identity_pages, f.memory, sizeof(identity_pages)) == 0);
  assert(h2_fm175xx_ntag_read_ndef(&reader, pages, sizeof(pages), &len) == 0);
  assert(len == sizeof(message) && memcmp(pages, message, len) == 0);
  unsigned saved_writes = f.writes;
  f.memory[15] = 15u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_UNAVAILABLE);
  assert(f.writes == saved_writes);
  f.memory[15] = 0u;
  f.memory[10] = 1u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_UNAVAILABLE);
  f.memory[10] = 0u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, pages, sizeof(pages)) ==
         H2_FM175XX_ERR_NO_MEMORY);
  f.reject_write = f.writes + 2u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) ==
         H2_FM175XX_ERR_PROTOCOL);
  assert(f.memory[16] == 3u && f.memory[17] == 0u);
  f.reject_write = 0u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, message, sizeof(message)) == 0);
  uint8_t changed[sizeof(message)];
  memcpy(changed, message, sizeof(changed));
  changed[sizeof(changed) - 1u] = 'y';
  f.discard_write = f.writes + 2u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, changed, sizeof(changed)) ==
         H2_FM175XX_OK);
  assert(memcmp(identity_pages, f.memory, sizeof(identity_pages)) == 0);
  f.discard_write = 0u;
  assert(h2_fm175xx_ntag_write_ndef(&reader, changed, sizeof(changed)) == 0);
  assert(h2_fm175xx_ntag_read_ndef(&reader, pages, sizeof(pages), &len) == 0);
  assert(len == sizeof(changed) && memcmp(pages, changed, len) == 0);
  /* An empty RX indication before the timer expires must leave the command
   * alive: the real card's ATQA, UID, SAK and page replies can follow it. */
  f.empty_before_reply = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &len) ==
         H2_FM175XX_OK);
  assert(len == 160u && memcmp(pages, f.memory, len) == 0);
  assert(f.delayed_replies > 10u);
  /* An empty FIFO cannot make stale RxLastBits into a received packet. */
  f.card = IDLE;
  f.empty_last_bits = 7u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  f.empty_last_bits = 0u;
  /* The genuine reply may arrive between the empty FIFO sample and IRQ
   * acknowledgement. The acknowledgement must not erase that new completion. */
  f.card = IDLE;
  f.reply_on_empty_probe = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &len) ==
         H2_FM175XX_OK);
  assert(len == 160u && memcmp(pages, f.memory, len) == 0);
  f.reply_on_empty_probe = 0;
  f.empty_before_reply = 0;
  /* A 3 ms reply misses the original hardware deadline but is received
   * under the BK controller's explicit 15 ms per-frame budget. */
  f.reply_delay_us = 3000u;
  f.card = IDLE;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_TIMEOUT);
  f.card = IDLE;
  assert(h2_fm175xx_type_a_activate_timed(&reader, &next, 15u) ==
         H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(h2_fm175xx_type_a_activate_timed(&reader, &next, 0u) ==
         H2_FM175XX_ERR_INVALID_ARG);
  assert(h2_fm175xx_type_a_activate_timed(&reader, &next, 51u) ==
         H2_FM175XX_ERR_INVALID_ARG);
  f.reply_delay_us = 0u;
  for (unsigned i = 0u; i < 3u; ++i)
    assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  f.card = HALT;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  /* A slow host can observe RX completion and timer expiry together. The
   * complete, valid reply still proves the card responded before inspection. */
  f.response_irq = 0x21u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  f.response_irq = 0u;
  f.card = ABSENT;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_TIMEOUT);
  /* An empty RX-complete indication is silence, not a malformed card reply. */
  f.silence_irq = 0x20u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_TIMEOUT);
  assert(next.uid_len == 0u);
  f.card = IDLE;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  f.card = ABSENT;
  f.silence_error = 0x04u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  /* Timer expiry must not hide a chip-reported error in completed RX. */
  f.silence_irq = 0x21u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  f.silence_irq = 0x20u;
  f.silence_error = 0u;
  f.unexpected_halt_reply = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  f.unexpected_halt_reply = 0;
  f.card = IDLE;
  f.short_atqa = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  f.short_atqa = 0;
  f.card = IDLE;
  f.short_sak_bits = 4;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  assert(next.uid_len == 0u);
  f.short_sak_bits = 0;
  f.card = IDLE;
  f.sak_crc_bytes = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  f.corrupt_sak_crc = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_PROTOCOL);
  assert(next.uid_len == 0u);
  f.corrupt_sak_crc = 0;
  f.sak_crc_bytes = 0;
  f.card = IDLE;
  f.idle_requires_reqa = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  /* Starting a short command must not depend on a FIFO-low interrupt that
   * may already have been acknowledged while the FIFO was empty. */
  f.card = IDLE;
  f.no_initial_tx_alert = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  f.no_initial_tx_alert = 0;
  /* The reader can reset independently of the MCU and its open handle. Its
   * analog defaults leave RF disabled; restoring only the software state
   * cannot make the retained, valid card respond. */
  f.card = IDLE;
  f.registers[H2_FM175XX_REG_TX_CTRL] = 0x80u;
  f.registers[H2_FM175XX_REG_RFCFG] = 0x48u;
  f.registers[H2_FM175XX_REG_RX_THRES] = 0x84u;
  /* A retained configuration can also route away from the analog receiver. */
  f.registers[H2_FM175XX_REG_RX_SEL] = 0x04u;
  const unsigned resets_before_recovery = f.soft_resets;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(f.soft_resets == resets_before_recovery);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(f.registers[H2_FM175XX_REG_TX_CTRL] == 0x83u);
  assert(f.registers[H2_FM175XX_REG_RFCFG] == 0x68u);
  assert(f.registers[H2_FM175XX_REG_RX_THRES] == 0x74u);
  assert(f.registers[H2_FM175XX_REG_RX_SEL] == 0x86u);
  /* A disconnected receiver input is an IO failure even when RF enable works.
   */
  f.card = IDLE;
  f.reject_receiver_select = 1;
  f.registers[H2_FM175XX_REG_TX_CTRL] = 0x80u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
  assert(next.uid_len == 0u);
  f.reject_receiver_select = 0;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  /* An unsuccessful RF-enable write is an actual hardware failure, not
   * successful initialization followed by an endless absent-card result. */
  f.card = IDLE;
  f.reject_rf_enable = 1;
  f.registers[H2_FM175XX_REG_TX_CTRL] = 0x80u;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
  assert(next.uid_len == 0u);
  f.reject_rf_enable = 0;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  f.truncate_fifo_write = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
  f.truncate_fifo_write = 0;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  /* Apply a board profile to an already-active reader, without resetting the
   * chip or weakening any UID/BCC/SAK validation. */
  f.reject_rf_gain = 1;
  assert(h2_fm175xx_type_a_activate_tuned(&reader, &next, 15u, 0x7fu, 0x84u) ==
         H2_FM175XX_ERR_IO);
  assert(next.uid_len == 0u);
  f.reject_rf_gain = 0;
  const unsigned resets_before_tuning = f.soft_resets;
  assert(h2_fm175xx_type_a_activate_tuned(&reader, &next, 15u, 0x7fu, 0x84u) ==
         H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(f.registers[H2_FM175XX_REG_RFCFG] == 0x7fu &&
         f.registers[H2_FM175XX_REG_RX_THRES] == 0x84u);
  assert(h2_fm175xx_type_a_activate_tuned(&reader, &next, 15u, 0x7fu, 0x84u) ==
         H2_FM175XX_OK);
  assert(f.soft_resets == resets_before_tuning);
  assert(h2_fm175xx_type_a_activate_tuned(&reader, &next, 0u, 0x7fu, 0x84u) ==
         H2_FM175XX_ERR_INVALID_ARG);
  f.reset_gsn = 0x98u;
  f.reset_gwgsp = 0x2au;
  const h2_fm175xx_type_a_profile_t profile = {
      .rf_config = 0x7fu, .rx_threshold = 0x84u, .preserve_tx_drive = 1u};
  assert(h2_fm175xx_open_type_a_profile(&reader, &profile) == H2_FM175XX_OK);
  const unsigned profile_resets = f.soft_resets;
  for (unsigned i = 0u; i < 2u; ++i) {
    assert(h2_fm175xx_type_a_activate_profile(&reader, &next, 15u, &profile) ==
           H2_FM175XX_OK);
    assert(memcmp(&first, &next, sizeof(first)) == 0);
    assert(f.registers[H2_FM175XX_REG_GSN] == f.reset_gsn &&
           f.registers[H2_FM175XX_REG_GWGSP] == f.reset_gwgsp);
  }
  assert(f.soft_resets == profile_resets);
  assert(h2_fm175xx_open_type_a(&reader) == H2_FM175XX_OK);
  assert(f.registers[H2_FM175XX_REG_GSN] == 0xf8u &&
         f.registers[H2_FM175XX_REG_GWGSP] == 0x3fu);
  f.fail_post_idle = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
  assert(next.uid_len == 0u);
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  f.fail_register = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_ERR_IO);
  return 0;
}
