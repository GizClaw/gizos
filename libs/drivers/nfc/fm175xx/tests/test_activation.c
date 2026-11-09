#include "h2_fm175xx.h"
#include "h2_fm175xx_defs.h"

#include <assert.h>
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
  uint8_t memory[160];
  unsigned halts;
  unsigned wakes;
  unsigned reads;
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
} fixture_t;

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

int main(void) {
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
  assert(len == sizeof(f.memory) && memcmp(pages, f.memory, len) == 0);
  assert(f.reads == 10u);
  /* An empty RX indication before the timer expires must leave the command
   * alive: the real card's ATQA, UID, SAK and page replies can follow it. */
  f.empty_before_reply = 1;
  assert(h2_fm175xx_type_a_activate(&reader, &next) == H2_FM175XX_OK);
  assert(memcmp(&first, &next, sizeof(first)) == 0);
  assert(h2_fm175xx_ntag_read_all(&reader, pages, sizeof(pages), &len) ==
         H2_FM175XX_OK);
  assert(len == sizeof(f.memory) && memcmp(pages, f.memory, len) == 0);
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
  assert(len == sizeof(f.memory) && memcmp(pages, f.memory, len) == 0);
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
