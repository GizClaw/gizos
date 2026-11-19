#include "h2_fm175xx.h"

#include "h2_fm175xx_defs.h"
#include "h2_sha256.h"

#include <stdbool.h>
#include <string.h>

static int write_reg(h2_fm175xx_t* reader, uint8_t reg, uint8_t value) {
  return reader->transport.write_reg(reader->transport.user, reg, value);
}

static int write_regs(h2_fm175xx_t* reader, uint8_t reg, const uint8_t* data,
                      size_t len) {
  if (len == 0u) {
    return H2_FM175XX_OK;
  }
  return reader->transport.write_regs(reader->transport.user, reg, data, len);
}

static int read_reg(h2_fm175xx_t* reader, uint8_t reg, uint8_t* out_value) {
  return reader->transport.read_reg(reader->transport.user, reg, out_value);
}

static int read_regs(h2_fm175xx_t* reader, uint8_t reg, uint8_t* out_data,
                     size_t len) {
  if (len == 0u) {
    return H2_FM175XX_OK;
  }
  return reader->transport.read_regs(reader->transport.user, reg, out_data,
                                     len);
}

static void sleep_ms(h2_fm175xx_t* reader, uint32_t ms) {
  if (reader->transport.sleep_ms != NULL) {
    reader->transport.sleep_ms(reader->transport.user, ms);
  }
}

static int set_bit_mask(h2_fm175xx_t* reader, uint8_t reg, uint8_t mask) {
  uint8_t value = 0;
  int rc = read_reg(reader, reg, &value);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  return write_reg(reader, reg, (uint8_t)(value | mask));
}

static int clear_bit_mask(h2_fm175xx_t* reader, uint8_t reg, uint8_t mask) {
  uint8_t value = 0;
  int rc = read_reg(reader, reg, &value);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  return write_reg(reader, reg, (uint8_t)(value & (uint8_t)~mask));
}

static int write_fifo(h2_fm175xx_t* reader, const uint8_t* data, size_t len) {
  return write_regs(reader, H2_FM175XX_REG_FIFO_DATA, data, len);
}

static int read_fifo(h2_fm175xx_t* reader, uint8_t* data, size_t len) {
  return read_regs(reader, H2_FM175XX_REG_FIFO_DATA, data, len);
}

static int clear_fifo(h2_fm175xx_t* reader) {
  int rc = set_bit_mask(reader, H2_FM175XX_REG_FIFO_LEVEL, 0x80u);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  uint8_t level = 0;
  rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &level);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  return level == 0u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
}

static int set_timer(h2_fm175xx_t* reader, uint32_t timeout_ms) {
  uint16_t prescaler = 1u;
  uint32_t reload = 0xffffu;
  while (prescaler < 0x0fffu) {
    reload = ((timeout_ms * 13560u) - 1u) / ((prescaler * 2u) + 1u);
    if (reload < 0x10000u) {
      break;
    }
    prescaler++;
  }

  int rc =
      set_bit_mask(reader, H2_FM175XX_REG_TMODE, (uint8_t)(prescaler >> 8));
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_TPRESCALER,
                   (uint8_t)(prescaler & 0xffu));
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_TRELOAD_HI,
                   (uint8_t)((reload >> 8) & 0xffu));
  }
  if (rc == H2_FM175XX_OK) {
    rc =
        write_reg(reader, H2_FM175XX_REG_TRELOAD_LO, (uint8_t)(reload & 0xffu));
  }
  return rc;
}

static int pre_cmd(h2_fm175xx_t* reader) {
  int rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
  if (rc == H2_FM175XX_OK) {
    rc = clear_fifo(reader);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_WATER_LEVEL, 0x20u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x7fu);
  }
  return rc;
}

static int post_cmd(h2_fm175xx_t* reader, int rc) {
  if (rc == H2_FM175XX_OK) {
    uint8_t error = 0;
    rc = read_reg(reader, H2_FM175XX_REG_ERROR, &error);
    if (rc == H2_FM175XX_OK && error != 0u) {
      rc = H2_FM175XX_ERR_PROTOCOL;
    }
  }
  int cleanup_rc = set_bit_mask(reader, H2_FM175XX_REG_CONTROL, 0x80u);
  const int idle_rc =
      write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
  if (cleanup_rc == H2_FM175XX_OK) cleanup_rc = idle_rc;
  const int framing_rc =
      clear_bit_mask(reader, H2_FM175XX_REG_BIT_FRAMING, 0x80u);
  if (cleanup_rc == H2_FM175XX_OK) cleanup_rc = framing_rc;
  /* A bus failure during cleanup must not publish a tag or become silence. */
  return cleanup_rc == H2_FM175XX_OK ? rc : cleanup_rc;
}

typedef struct rx_observation {
  int result;
  uint8_t irq, error, fifo, control;
} rx_observation_t;

static int transceive_observe(h2_fm175xx_t* reader, const uint8_t* in,
                              size_t in_len, uint8_t* out, size_t out_cap,
                              size_t* out_bits, rx_observation_t* observation) {
  if ((in_len > 0u && in == NULL) || out == NULL || out_bits == NULL) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }

  int rc = pre_cmd(reader);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  /* Prime the FIFO before starting RF. FIFO-low is a refill indication, not
   * a prerequisite for sending the first bytes of a short command. */
  if (in_len != 0u) {
    const size_t chunk = in_len > 32u ? 32u : in_len;
    rc = write_fifo(reader, in, chunk);
    if (rc != H2_FM175XX_OK) return post_cmd(reader, rc);
    uint8_t loaded = 0u;
    rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &loaded);
    if (rc != H2_FM175XX_OK) return post_cmd(reader, rc);
    if (loaded != chunk) return post_cmd(reader, H2_FM175XX_ERR_IO);
    in += chunk;
    in_len -= chunk;
  }
  rc = set_bit_mask(reader, H2_FM175XX_REG_TMODE, 0x80u);
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_TRANSCEIVE);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_BIT_FRAMING, 0x80u);
  }
  if (rc != H2_FM175XX_OK) {
    return post_cmd(reader, rc);
  }

  size_t out_len = 0u;
  for (uint32_t guard = 0; guard < 100u; ++guard) {
    uint8_t irq = 0;
    rc = read_reg(reader, H2_FM175XX_REG_COMIRQ, &irq);
    if (rc != H2_FM175XX_OK) {
      return post_cmd(reader, rc);
    }
    /* On a slow host RX completion and timer expiry can be observed together.
     * Validate the completed reply before treating the timer as no response. */
    if ((irq & 0x01u) != 0u && ((irq & 0x20u) == 0u || in_len != 0u)) {
      return post_cmd(reader, H2_FM175XX_ERR_TIMEOUT);
    }
    if ((irq & 0x04u) != 0u && in_len > 0u) {
      const size_t chunk = in_len > 32u ? 32u : in_len;
      rc = write_fifo(reader, in, chunk);
      if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
      }
      in += chunk;
      in_len -= chunk;
      rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x04u);
      if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
      }
    }
    if ((irq & 0x08u) != 0u && (irq & 0x40u) != 0u && in_len == 0u) {
      uint8_t fifo_level = 0;
      rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &fifo_level);
      if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
      }
      if (fifo_level > 32u) {
        if (out_len + 32u > out_cap) {
          return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
        }
        rc = read_fifo(reader, out + out_len, 32u);
        if (rc != H2_FM175XX_OK) {
          return post_cmd(reader, rc);
        }
        out_len += 32u;
        rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x08u);
        if (rc != H2_FM175XX_OK) {
          return post_cmd(reader, rc);
        }
      }
    }
    if ((irq & 0x20u) != 0u && in_len == 0u) {
      /* Acknowledge the observed completion before inspecting FIFO state.
       * A later real packet must retain its own RX interrupt. */
      rc = write_reg(reader, H2_FM175XX_REG_COMIRQ, 0x20u);
      if (rc != H2_FM175XX_OK) return post_cmd(reader, rc);
      uint8_t fifo_level = 0;
      uint8_t last_bits = 0;
      rc = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &fifo_level);
      if (rc == H2_FM175XX_OK) {
        rc = read_reg(reader, H2_FM175XX_REG_CONTROL, &last_bits);
      }
      if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
      }
      if (observation != NULL) {
        observation->irq = irq;
        observation->fifo = fifo_level;
        observation->control = last_bits;
        observation->result =
            read_reg(reader, H2_FM175XX_REG_ERROR, &observation->error);
      }
      last_bits &= 0x07u;
      if (fifo_level == 0u && out_len == 0u) {
        uint8_t error = 0u;
        rc = read_reg(reader, H2_FM175XX_REG_ERROR, &error);
        if (rc != H2_FM175XX_OK) return post_cmd(reader, rc);
        if (error != 0u) return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
        if ((irq & 0x01u) != 0u)
          return post_cmd(reader, H2_FM175XX_ERR_TIMEOUT);
        /* Without FIFO bytes RxLastBits can still describe an older packet.
         * It is neither received data nor a malformed new reply. Do not clear
         * RX again here: a real packet may have arrived during these reads. */
        sleep_ms(reader, 1u);
        continue;
      }
      if (fifo_level == 0u && last_bits > 0u)
        return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
      if (out_len + fifo_level > out_cap) {
        return post_cmd(reader, H2_FM175XX_ERR_PROTOCOL);
      }
      rc = read_fifo(reader, out + out_len, fifo_level);
      if (rc != H2_FM175XX_OK) {
        return post_cmd(reader, rc);
      }
      out_len += fifo_level;
      *out_bits =
          last_bits > 0u ? ((out_len - 1u) * 8u) + last_bits : out_len * 8u;
      return post_cmd(reader, H2_FM175XX_OK);
    }
    sleep_ms(reader, 1u);
  }
  return post_cmd(reader, H2_FM175XX_ERR_TIMEOUT);
}

static int transceive(h2_fm175xx_t* reader, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t out_cap, size_t* out_bits) {
  return transceive_observe(reader, in, in_len, out, out_cap, out_bits, NULL);
}

static int type_a_halt(h2_fm175xx_t* reader, uint32_t timeout_ms,
                       size_t* out_bits) {
  int rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    /* HLTA has no reply. Only TX needs CRC; leave RX unfiltered so even an
     * unexpected short reply is visible and rejected instead of stripped. */
    rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_timer(reader, timeout_ms);
  }
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  const uint8_t halt[2] = {0x50u, 0x00u};
  uint8_t response[1];
  size_t bits = 0u;
  rc =
      transceive(reader, halt, sizeof(halt), response, sizeof(response), &bits);
  if (out_bits != NULL) *out_bits = bits;
  /* HLTA has no response. A previously selected tag enters HALT; tags
   * already in IDLE/HALT ignore it. Bus failures must still propagate. */
  return rc == H2_FM175XX_ERR_TIMEOUT
             ? H2_FM175XX_OK
             : (rc == H2_FM175XX_OK ? H2_FM175XX_ERR_PROTOCOL : rc);
}

static int type_a_request(h2_fm175xx_t* reader, uint32_t timeout_ms,
                          uint8_t out_atqa[2],
                          h2_fm175xx_activation_diagnostics_t* diagnostics) {
  int rc = clear_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
  if (rc == H2_FM175XX_OK) {
    rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x07u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_timer(reader, timeout_ms);
  }
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  /* Poll IDLE cards with REQA first. A preceding selected card is in HALT
   * and needs WUPA; only an actual timeout permits that second attempt. */
  const uint8_t requests[] = {0x26u, 0x52u};
  for (size_t i = 0u; i < sizeof(requests); ++i) {
    size_t bits = 0u;
    rc = transceive(reader, &requests[i], 1u, out_atqa, 2u, &bits);
    if (diagnostics != NULL) {
      diagnostics->command = requests[i];
      diagnostics->reply_bits = bits;
    }
    if (rc == H2_FM175XX_ERR_TIMEOUT && i == 0u) continue;
    if (rc != H2_FM175XX_OK) return rc;
    return bits == 16u ? H2_FM175XX_OK : H2_FM175XX_ERR_PROTOCOL;
  }
  return H2_FM175XX_ERR_TIMEOUT;
}

static int type_a_anticollision(h2_fm175xx_t* reader, uint32_t timeout_ms,
                                uint8_t selcode, uint8_t out_uid[5],
                                size_t* out_bits) {
  int rc = clear_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
  if (rc == H2_FM175XX_OK) {
    rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_COLL, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_timer(reader, timeout_ms);
  }
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  const uint8_t send[2] = {selcode, 0x20u};
  uint8_t recv[5] = {0};
  size_t bits = 0u;
  rc = transceive(reader, send, sizeof(send), recv, sizeof(recv), &bits);
  if (out_bits != NULL) *out_bits = bits;
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  if (bits != 40u ||
      recv[4] != (uint8_t)(recv[0] ^ recv[1] ^ recv[2] ^ recv[3])) {
    return H2_FM175XX_ERR_PROTOCOL;
  }
  memcpy(out_uid, recv, sizeof(recv));
  return H2_FM175XX_OK;
}

static int type_a_check_reply(const uint8_t* reply, size_t length,
                              size_t bits) {
  if (bits == length * 8u) return H2_FM175XX_OK;
  if (bits != (length + 2u) * 8u) return H2_FM175XX_ERR_PROTOCOL;
  uint16_t crc = 0x6363u;
  for (size_t index = 0u; index < length; ++index) {
    crc ^= reply[index];
    for (unsigned bit = 0u; bit < 8u; ++bit)
      crc = (uint16_t)((crc >> 1u) ^ ((crc & 1u) ? 0x8408u : 0u));
  }
  return reply[length] == (uint8_t)crc &&
                 reply[length + 1u] == (uint8_t)(crc >> 8u)
             ? H2_FM175XX_OK
             : H2_FM175XX_ERR_PROTOCOL;
}

static int type_a_select(h2_fm175xx_t* reader, uint32_t timeout_ms,
                         uint8_t selcode, const uint8_t uid[5],
                         uint8_t* out_sak, size_t* out_bits) {
  uint8_t send[7] = {selcode, 0x70u, uid[0], uid[1], uid[2], uid[3], uid[4]};
  uint8_t recv[5] = {0};
  size_t bits = 0u;
  int rc = write_reg(reader, H2_FM175XX_REG_BIT_FRAMING, 0x00u);
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_TX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = clear_bit_mask(reader, H2_FM175XX_REG_STATUS2, 0x08u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_timer(reader, timeout_ms);
  }
  if (rc == H2_FM175XX_OK) {
    rc = transceive(reader, send, sizeof(send), recv, sizeof(recv), &bits);
  }
  if (out_bits != NULL) *out_bits = bits;
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  /* Some family members strip the checked CRC; others retain its two bytes.
   * A short MIFARE NAK or a partial packet is never a successful selection. */
  rc = type_a_check_reply(recv, 1u, bits);
  if (rc != H2_FM175XX_OK) return rc;
  *out_sak = recv[0];
  return H2_FM175XX_OK;
}

static int ntag_read_once_diagnostic(
    h2_fm175xx_t* reader, uint8_t page_addr, uint8_t out[16],
    h2_fm175xx_read_diagnostics_t* diagnostics) {
  const uint8_t send[2] = {0x30u, page_addr};
  uint8_t reply[18];
  size_t bits = 0u;
  rx_observation_t observation = {.result = H2_FM175XX_ERR_UNAVAILABLE};
  int rc = set_timer(reader, 5u);
  if (rc == H2_FM175XX_OK) {
    rc = transceive_observe(reader, send, sizeof(send), reply, sizeof(reply),
                            &bits, diagnostics == NULL ? NULL : &observation);
  }
  int transfer_result = rc;
  if (rc == H2_FM175XX_OK) rc = type_a_check_reply(reply, 16u, bits);
  if (diagnostics != NULL) {
    ++diagnostics->read_count;
    if (bits == 144u) ++diagnostics->retained_crc_frames;
    diagnostics->last_reply_bits = bits;
    diagnostics->last_page = page_addr;
    diagnostics->last_result = rc;
    diagnostics->transfer_result = transfer_result;
    diagnostics->short_reply_valid =
        (uint8_t)(transfer_result == H2_FM175XX_OK && bits == 4u);
    diagnostics->short_reply_low4 =
        diagnostics->short_reply_valid ? (uint8_t)(reply[0] & 15u) : 0u;
    diagnostics->rx_status_result = observation.result;
    diagnostics->rx_irq = observation.irq;
    diagnostics->rx_error = observation.error;
    diagnostics->rx_fifo = observation.fifo;
    diagnostics->rx_control = observation.control;
    if (rc != H2_FM175XX_OK) {
      int status =
          read_reg(reader, H2_FM175XX_REG_COMIRQ, &diagnostics->last_irq);
      if (status == H2_FM175XX_OK)
        status =
            read_reg(reader, H2_FM175XX_REG_ERROR, &diagnostics->last_error);
      if (status == H2_FM175XX_OK)
        status = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL,
                          &diagnostics->last_fifo);
      if (status == H2_FM175XX_OK)
        status = read_reg(reader, H2_FM175XX_REG_CONTROL,
                          &diagnostics->last_control);
      diagnostics->status_result = status;
    }
  }
  if (rc == H2_FM175XX_OK) memcpy(out, reply, 16u);
  return rc;
}

static int ntag_read_once(h2_fm175xx_t* reader, uint8_t page_addr,
                          uint8_t out[16]) {
  return ntag_read_once_diagnostic(reader, page_addr, out, NULL);
}

int h2_fm175xx_init(h2_fm175xx_t* reader,
                    const h2_fm175xx_transport_t* transport) {
  if (reader == NULL || transport == NULL || transport->write_reg == NULL ||
      transport->write_regs == NULL || transport->read_reg == NULL ||
      transport->read_regs == NULL) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  reader->transport = *transport;
  return H2_FM175XX_OK;
}

static int configure_type_a(h2_fm175xx_t* reader, uint8_t rf_config,
                            uint8_t rx_threshold, int preserve_tx_drive) {
  int rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
  sleep_ms(reader, 2u);
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_CONTROL, 0x10u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = set_bit_mask(reader, H2_FM175XX_REG_TX_AUTO, 0x40u);
  }
  if (rc == H2_FM175XX_OK) {
    /* CRC_A uses the 0x6363 preset. Do not inherit a reset/default preset. */
    rc = write_reg(reader, H2_FM175XX_REG_MODE, 0x3du);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_TX_MODE, 0x00u);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_RX_MODE, 0x00u);
  }
  if (rc == H2_FM175XX_OK) {
    /* Select the analog receiver input and the Type A receive-wait setting.
     * Do not inherit another protocol's input selection. */
    rc = write_reg(reader, H2_FM175XX_REG_RX_SEL, 0x86u);
  }
  if (rc == H2_FM175XX_OK) {
    uint8_t selection = 0u;
    rc = read_reg(reader, H2_FM175XX_REG_RX_SEL, &selection);
    if (rc == H2_FM175XX_OK && selection != 0x86u) rc = H2_FM175XX_ERR_IO;
  }
  if (rc == H2_FM175XX_OK && !preserve_tx_drive) {
    rc = write_reg(reader, H2_FM175XX_REG_GSN, 0xf8u);
  }
  if (rc == H2_FM175XX_OK && !preserve_tx_drive) {
    rc = write_reg(reader, H2_FM175XX_REG_GWGSP, 0x3fu);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_RFCFG, rf_config);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_RX_THRES, rx_threshold);
  }
  if (rc == H2_FM175XX_OK) {
    rc = write_reg(reader, H2_FM175XX_REG_TX_CTRL, 0x83u);
  }
  if (rc == H2_FM175XX_OK) {
    /* A newly powered passive tag needs field-settling time before HLTA/WUPA.
     */
    sleep_ms(reader, 5u);
    uint8_t tx_control = 0u;
    rc = read_reg(reader, H2_FM175XX_REG_TX_CTRL, &tx_control);
    if (rc == H2_FM175XX_OK && (tx_control & 0x03u) != 0x03u)
      rc = H2_FM175XX_ERR_IO;
  }
  return rc;
}

static int open_type_a(h2_fm175xx_t* reader, uint8_t rf_config,
                       uint8_t rx_threshold, int preserve_tx_drive) {
  if (reader == NULL) return H2_FM175XX_ERR_INVALID_ARG;
  int rc = write_reg(reader, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_SOFT_RESET);
  if (rc != H2_FM175XX_OK) return rc;
  sleep_ms(reader, 2u);
  return configure_type_a(reader, rf_config, rx_threshold, preserve_tx_drive);
}

int h2_fm175xx_open_type_a(h2_fm175xx_t* reader) {
  return open_type_a(reader, 0x68u, 0x74u, 0);
}

int h2_fm175xx_open_type_a_profile(h2_fm175xx_t* reader,
                                   const h2_fm175xx_type_a_profile_t* profile) {
  if (profile == NULL || profile->preserve_tx_drive > 1u)
    return H2_FM175XX_ERR_INVALID_ARG;
  return open_type_a(reader, profile->rf_config, profile->rx_threshold,
                     profile->preserve_tx_drive);
}

int h2_fm175xx_type_a_activate(h2_fm175xx_t* reader,
                               h2_fm175xx_type_a_card_t* out_card) {
  return h2_fm175xx_type_a_activate_timed(reader, out_card, 1u);
}

static int activate_type_a(h2_fm175xx_t* reader,
                           h2_fm175xx_type_a_card_t* out_card,
                           uint32_t timeout_ms, uint8_t rf_config,
                           uint8_t rx_threshold, int retune,
                           int preserve_tx_drive,
                           h2_fm175xx_activation_diagnostics_t* diagnostics) {
  if (reader == NULL || out_card == NULL || timeout_ms == 0u ||
      timeout_ms > 50u) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  memset(out_card, 0, sizeof(*out_card));
  uint8_t tx_control = 0u;
  int rc = read_reg(reader, H2_FM175XX_REG_TX_CTRL, &tx_control);
  if (rc != H2_FM175XX_OK) return rc;
  /* The device can reset independently of its host-side handle. A disabled
   * field must restore the complete Type A setup, not masquerade as no tag. */
  if ((tx_control & 0x03u) != 0x03u || retune) {
    /* RF cycling is not a request to reset the reader on every poll. */
    rc = configure_type_a(reader, rf_config, rx_threshold, preserve_tx_drive);
    if (rc != H2_FM175XX_OK) return rc;
    if (retune) {
      uint8_t gain = 0u, threshold = 0u;
      rc = read_reg(reader, H2_FM175XX_REG_RFCFG, &gain);
      if (rc == H2_FM175XX_OK)
        rc = read_reg(reader, H2_FM175XX_REG_RX_THRES, &threshold);
      if (rc != H2_FM175XX_OK) return rc;
      /* Compare defined gain/decoder fields, excluding reserved bits. */
      if ((gain & 0x70u) != (rf_config & 0x70u) ||
          (threshold & 0xf7u) != (rx_threshold & 0xf7u))
        return H2_FM175XX_ERR_IO;
    }
  }
  /* Polling and content reads share the reader. The preceding scan leaves
   * its tag ACTIVE, where REQA is ignored. End that selection, then WUPA
   * wakes both a newly presented IDLE tag and the retained HALT tag. */
  if (diagnostics != NULL) {
    diagnostics->phase = H2_FM175XX_ACTIVATION_HALT;
    diagnostics->command = 0x50u;
  }
  rc = type_a_halt(reader, timeout_ms,
                   diagnostics == NULL ? NULL : &diagnostics->reply_bits);
  if (rc == H2_FM175XX_OK) {
    if (diagnostics != NULL) {
      diagnostics->phase = H2_FM175XX_ACTIVATION_REQUEST;
      diagnostics->command = 0u;
      diagnostics->reply_bits = 0u;
    }
    rc = type_a_request(reader, timeout_ms, out_card->atqa, diagnostics);
  }
  if (rc != H2_FM175XX_OK) {
    return rc;
  }

  const uint8_t cascade = (uint8_t)(out_card->atqa[0] & 0xc0u);
  const uint8_t levels =
      cascade == 0x00u ? 1u
                       : (cascade == 0x40u ? 2u : (cascade == 0x80u ? 3u : 0u));
  if (levels == 0u) {
    return H2_FM175XX_ERR_PROTOCOL;
  }

  static const uint8_t selcodes[3] = {0x93u, 0x95u, 0x97u};
  for (uint8_t i = 0u; i < levels; ++i) {
    uint8_t* uid = out_card->uid + (i * 5u);
    if (diagnostics != NULL) {
      diagnostics->phase = H2_FM175XX_ACTIVATION_ANTICOLLISION;
      diagnostics->command = selcodes[i];
      diagnostics->cascade_level = (uint8_t)(i + 1u);
      diagnostics->reply_bits = 0u;
    }
    rc = type_a_anticollision(
        reader, timeout_ms, selcodes[i], uid,
        diagnostics == NULL ? NULL : &diagnostics->reply_bits);
    if (rc != H2_FM175XX_OK) {
      return rc;
    }
    if (diagnostics != NULL) {
      diagnostics->phase = H2_FM175XX_ACTIVATION_SELECT;
      diagnostics->reply_bits = 0u;
    }
    rc = type_a_select(reader, timeout_ms, selcodes[i], uid, &out_card->sak[i],
                       diagnostics == NULL ? NULL : &diagnostics->reply_bits);
    if (rc != H2_FM175XX_OK) {
      return rc;
    }
  }
  out_card->uid_len = (uint8_t)(levels * 5u);
  out_card->sak_len = levels;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_ACTIVATION_COMPLETE;
  return H2_FM175XX_OK;
}

int h2_fm175xx_type_a_activate_diagnostic(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* out_card,
    h2_fm175xx_activation_diagnostics_t* diagnostics) {
  if (diagnostics != NULL) {
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->status_result = H2_FM175XX_ERR_UNAVAILABLE;
  }
  int rc =
      activate_type_a(reader, out_card, 1u, 0x68u, 0x74u, 0, 0, diagnostics);
  if (diagnostics != NULL && reader != NULL && rc != H2_FM175XX_OK &&
      rc != H2_FM175XX_ERR_INVALID_ARG) {
    int status =
        read_reg(reader, H2_FM175XX_REG_COMIRQ, &diagnostics->last_irq);
    if (status == H2_FM175XX_OK)
      status = read_reg(reader, H2_FM175XX_REG_ERROR, &diagnostics->last_error);
    if (status == H2_FM175XX_OK)
      status =
          read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL, &diagnostics->last_fifo);
    if (status == H2_FM175XX_OK)
      status =
          read_reg(reader, H2_FM175XX_REG_CONTROL, &diagnostics->last_control);
    diagnostics->status_result = status;
  }
  return rc;
}

int h2_fm175xx_type_a_activate_timed(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* out_card,
                                     uint32_t timeout_ms) {
  return activate_type_a(reader, out_card, timeout_ms, 0x68u, 0x74u, 0, 0,
                         NULL);
}

int h2_fm175xx_type_a_activate_tuned(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* out_card,
                                     uint32_t timeout_ms, uint8_t rf_config,
                                     uint8_t rx_threshold) {
  return activate_type_a(reader, out_card, timeout_ms, rf_config, rx_threshold,
                         1, 0, NULL);
}

int h2_fm175xx_type_a_activate_profile(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* out_card,
    uint32_t timeout_ms, const h2_fm175xx_type_a_profile_t* profile) {
  if (profile == NULL || profile->preserve_tx_drive > 1u)
    return H2_FM175XX_ERR_INVALID_ARG;
  return activate_type_a(reader, out_card, timeout_ms, profile->rf_config,
                         profile->rx_threshold, 1, profile->preserve_tx_drive,
                         NULL);
}

static int ntag_read_all(h2_fm175xx_t* reader, uint8_t* out_data,
                         size_t capacity, size_t* out_len,
                         h2_fm175xx_read_diagnostics_t* diagnostics) {
  if (diagnostics != NULL) {
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->last_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->transfer_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->status_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->rx_status_result = H2_FM175XX_ERR_UNAVAILABLE;
  }
  if (reader == NULL || out_data == NULL || out_len == NULL || capacity < 16u) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  int rc = ntag_read_once_diagnostic(reader, 0u, out_data, diagnostics);
  if (rc != H2_FM175XX_OK) {
    return rc;
  }
  const size_t total_len = 16u + (8u * out_data[14]);
  if (diagnostics != NULL) diagnostics->required_capacity = total_len;
  if (total_len > 1024u) return H2_FM175XX_ERR_UNAVAILABLE;
  if (total_len > capacity) {
    return H2_FM175XX_ERR_NO_MEMORY;
  }
  for (size_t offset = 16u; offset < total_len; offset += 16u) {
    uint8_t page[16];
    rc = ntag_read_once_diagnostic(reader, (uint8_t)(offset / 4u), page,
                                   diagnostics);
    if (rc != H2_FM175XX_OK) {
      return rc;
    }
    const size_t remaining = total_len - offset;
    memcpy(out_data + offset, page,
           remaining < sizeof(page) ? remaining : sizeof(page));
  }
  *out_len = total_len;
  return H2_FM175XX_OK;
}

int h2_fm175xx_ntag_read_all(h2_fm175xx_t* reader, uint8_t* out_data,
                             size_t capacity, size_t* out_len) {
  return ntag_read_all(reader, out_data, capacity, out_len, NULL);
}

static int ntag_read_until(h2_fm175xx_t* reader, uint8_t* raw, size_t size,
                           size_t* filled, size_t needed,
                           h2_fm175xx_read_diagnostics_t* diagnostics) {
  if (needed > size) return H2_FM175XX_ERR_PROTOCOL;
  while (*filled < needed) {
    uint8_t page[16];
    int rc = ntag_read_once_diagnostic(reader, (uint8_t)(*filled / 4u), page,
                                       diagnostics);
    if (rc != H2_FM175XX_OK) return rc;
    size_t length = size - *filled;
    if (length > sizeof(page)) length = sizeof(page);
    memcpy(raw + *filled, page, length);
    *filled += length;
  }
  return H2_FM175XX_OK;
}

int h2_fm175xx_ntag_read_ndef(h2_fm175xx_t* reader, uint8_t* out_data,
                              size_t capacity, size_t* out_len) {
  return h2_fm175xx_ntag_read_ndef_diagnostic(reader, out_data, capacity,
                                              out_len, NULL);
}

int h2_fm175xx_ntag_read_ndef_diagnostic(
    h2_fm175xx_t* reader, uint8_t* out_data, size_t capacity, size_t* out_len,
    h2_fm175xx_read_diagnostics_t* diagnostics) {
  if (diagnostics != NULL) {
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->status_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->rx_status_result = H2_FM175XX_ERR_UNAVAILABLE;
  }
  if (reader == NULL || out_data == NULL || out_len == NULL)
    return H2_FM175XX_ERR_INVALID_ARG;
  *out_len = 0u;
  uint8_t raw[1024];
  int rc = ntag_read_once_diagnostic(reader, 0u, raw, diagnostics);
  if (rc != H2_FM175XX_OK) return rc;
  if (raw[12] != 0xe1u || (raw[13] >> 4u) != 1u || (raw[15] >> 4u) != 0u)
    return H2_FM175XX_ERR_UNAVAILABLE;
  size_t size = 16u + 8u * raw[14];
  if (size > sizeof(raw)) return H2_FM175XX_ERR_UNAVAILABLE;
  size_t filled = 16u;
  for (size_t offset = 16u; offset < size;) {
    rc = ntag_read_until(reader, raw, size, &filled, offset + 1u, diagnostics);
    if (rc != H2_FM175XX_OK) return rc;
    uint8_t type = raw[offset++];
    if (type == 0u) continue;
    if (type == 0xfeu) break;
    if (offset == size) return H2_FM175XX_ERR_PROTOCOL;
    rc = ntag_read_until(reader, raw, size, &filled, offset + 1u, diagnostics);
    if (rc != H2_FM175XX_OK) return rc;
    size_t length = raw[offset++];
    if (length == 255u) {
      if (size - offset < 2u) return H2_FM175XX_ERR_PROTOCOL;
      rc =
          ntag_read_until(reader, raw, size, &filled, offset + 2u, diagnostics);
      if (rc != H2_FM175XX_OK) return rc;
      length = ((size_t)raw[offset] << 8u) | raw[offset + 1u];
      offset += 2u;
    }
    if (length > size - offset) return H2_FM175XX_ERR_PROTOCOL;
    if (type == 3u) {
      if (length == 0u) return H2_FM175XX_ERR_UNAVAILABLE;
      if (diagnostics != NULL) diagnostics->required_capacity = length;
      if (length > capacity) return H2_FM175XX_ERR_NO_MEMORY;
      rc = ntag_read_until(reader, raw, size, &filled, offset + length,
                           diagnostics);
      if (rc != H2_FM175XX_OK) return rc;
      memcpy(out_data, raw + offset, length);
      *out_len = length;
      return H2_FM175XX_OK;
    }
    offset += length;
  }
  return H2_FM175XX_ERR_UNAVAILABLE;
}

static int ntag_write_page(h2_fm175xx_t* reader, uint8_t page,
                           const uint8_t data[4],
                           h2_fm175xx_write_diagnostics_t* diagnostics) {
  const uint8_t command[] = {0xa2u, page, data[0], data[1], data[2], data[3]};
  uint8_t reply[3] = {0};
  size_t bits = 0u;
  rx_observation_t observation = {.result = H2_FM175XX_ERR_UNAVAILABLE};
  int rc = clear_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  if (rc == H2_FM175XX_OK && diagnostics != NULL) {
    int config =
        read_reg(reader, H2_FM175XX_REG_TX_MODE, &diagnostics->last_tx_mode);
    if (config == H2_FM175XX_OK)
      config =
          read_reg(reader, H2_FM175XX_REG_RX_MODE, &diagnostics->last_rx_mode);
    if (config == H2_FM175XX_OK)
      config = read_reg(reader, H2_FM175XX_REG_BIT_FRAMING,
                        &diagnostics->last_framing);
    diagnostics->configuration_result = config;
  }
  if (rc == H2_FM175XX_OK) rc = set_timer(reader, 10u);
  if (rc == H2_FM175XX_OK)
    rc = transceive_observe(reader, command, sizeof(command), reply,
                            sizeof(reply), &bits,
                            diagnostics == NULL ? NULL : &observation);
  int restored = set_bit_mask(reader, H2_FM175XX_REG_RX_MODE, 0x80u);
  int crc_valid =
      bits == 24u && type_a_check_reply(reply, 1u, bits) == H2_FM175XX_OK;
  int acknowledged =
      (bits == 4u && (reply[0] & 15u) == 10u) || (crc_valid && reply[0] == 10u);
  int result = rc != H2_FM175XX_OK         ? rc
               : restored != H2_FM175XX_OK ? restored
               : acknowledged              ? H2_FM175XX_OK
                                           : H2_FM175XX_ERR_PROTOCOL;
  if (diagnostics != NULL) {
    diagnostics->last_reply_bits = bits;
    diagnostics->last_reply_low4 =
        bits == 4u || (crc_valid && reply[0] <= 15u) ? reply[0] & 15u : 0u;
    diagnostics->last_reply_crc_valid = (uint8_t)crc_valid;
    memcpy(diagnostics->last_reply, reply, sizeof(reply));
    diagnostics->rx_status_result = observation.result;
    diagnostics->rx_irq = observation.irq;
    diagnostics->rx_error = observation.error;
    diagnostics->rx_fifo = observation.fifo;
    diagnostics->rx_control = observation.control;
    diagnostics->last_command_result = rc;
    diagnostics->last_restore_result = restored;
    if (result != H2_FM175XX_OK) {
      int status =
          read_reg(reader, H2_FM175XX_REG_COMIRQ, &diagnostics->last_irq);
      if (status == H2_FM175XX_OK)
        status =
            read_reg(reader, H2_FM175XX_REG_ERROR, &diagnostics->last_error);
      if (status == H2_FM175XX_OK)
        status = read_reg(reader, H2_FM175XX_REG_FIFO_LEVEL,
                          &diagnostics->last_fifo);
      if (status == H2_FM175XX_OK)
        status = read_reg(reader, H2_FM175XX_REG_CONTROL,
                          &diagnostics->last_control);
      diagnostics->status_result = status;
    }
  }
  return result;
}

int h2_fm175xx_ntag_write_ndef(h2_fm175xx_t* reader, const uint8_t* data,
                               size_t length) {
  return h2_fm175xx_ntag_write_ndef_diagnostic(reader, data, length, NULL);
}

static int write_ndef_page(h2_fm175xx_t* reader, uint8_t page,
                           const uint8_t data[4],
                           h2_fm175xx_write_diagnostics_t* diagnostics) {
  if (diagnostics != NULL) {
    ++diagnostics->write_calls;
    diagnostics->last_page = page;
  }
  return ntag_write_page(reader, page, data, diagnostics);
}

typedef struct ntag_write_layout {
  size_t ndef_offset;
  size_t previous_end;
  size_t previous_value_offset, previous_length;
  uint8_t previous_ndef_present;
  uint8_t protected_pages[32];
} ntag_write_layout_t;

static int ntag_pages_protected(const uint8_t* protected_pages, size_t start,
                                size_t end) {
  for (size_t page = start / 4u; page < (end + 3u) / 4u; ++page) {
    if ((protected_pages[page / 8u] & (uint8_t)(1u << (page % 8u))) != 0u)
      return 1;
  }
  return 0;
}

static int ntag_check_dynamic_locks(h2_fm175xx_t* reader, const uint8_t* raw,
                                    size_t size, size_t start, size_t bits) {
  uint8_t block[16];
  size_t cached = 1024u;
  for (size_t index = 0u; index < (bits + 7u) / 8u; ++index) {
    size_t address = start + index;
    uint8_t value;
    if (address < size) {
      value = raw[address];
    } else {
      size_t base = address & ~(size_t)15u;
      if (cached != base) {
        int rc = ntag_read_once(reader, (uint8_t)(base / 4u), block);
        if (rc != H2_FM175XX_OK) return rc;
        cached = base;
      }
      value = block[address - cached];
    }
    size_t remaining = bits - index * 8u;
    uint8_t mask = remaining < 8u ? (uint8_t)((1u << remaining) - 1u) : 255u;
    if ((value & mask) != 0u) return H2_FM175XX_ERR_UNAVAILABLE;
  }
  return H2_FM175XX_OK;
}

static int ntag_control_tlv(h2_fm175xx_t* reader, const uint8_t* raw,
                            size_t size, uint8_t type, size_t offset,
                            ntag_write_layout_t* layout) {
  const uint8_t* control = raw + offset;
  size_t page_size = (size_t)1u << (control[2] & 15u);
  size_t byte_offset = control[0] & 15u;
  if (byte_offset >= page_size) return H2_FM175XX_ERR_PROTOCOL;
  size_t start = (control[0] >> 4u) * page_size + byte_offset;
  size_t count = control[1] == 0u ? 256u : control[1];
  size_t length = type == 1u ? (count + 7u) / 8u : count;
  if (type == 2u && (control[2] & 0xf0u) != 0u) return H2_FM175XX_ERR_PROTOCOL;
  if (start >= 1024u || length > 1024u - start)
    return H2_FM175XX_ERR_UNAVAILABLE;
  if (start < offset + 3u) return H2_FM175XX_ERR_PROTOCOL;
  if (type == 1u) {
    int rc = ntag_check_dynamic_locks(reader, raw, size, start, count);
    if (rc != H2_FM175XX_OK) return rc;
  }
  for (size_t page = start / 4u; page < (start + length + 3u) / 4u; ++page)
    layout->protected_pages[page / 8u] |= (uint8_t)(1u << (page % 8u));
  return H2_FM175XX_OK;
}

static int ntag_find_write_layout(h2_fm175xx_t* reader, const uint8_t* raw,
                                  size_t size, ntag_write_layout_t* layout,
                                  h2_fm175xx_write_diagnostics_t* diagnostics) {
  *layout = (ntag_write_layout_t){.ndef_offset = 16u, .previous_end = 16u};
  for (size_t offset = 16u; offset < size;) {
    size_t start = offset;
    if (ntag_pages_protected(layout->protected_pages, offset, offset + 1u))
      return H2_FM175XX_ERR_UNAVAILABLE;
    uint8_t type = raw[offset++];
    if (type == 0u) continue;
    if (type == 0xfeu) {
      layout->ndef_offset = start;
      layout->previous_end = start;
      return H2_FM175XX_OK;
    }
    if (type != 1u && type != 2u && type != 3u)
      return H2_FM175XX_ERR_UNAVAILABLE;
    if (offset == size) return H2_FM175XX_ERR_PROTOCOL;
    size_t length = raw[offset++];
    if (type != 3u && length != 3u) return H2_FM175XX_ERR_PROTOCOL;
    if (length == 255u) {
      if (size - offset < 2u) return H2_FM175XX_ERR_PROTOCOL;
      length = ((size_t)raw[offset] << 8u) | raw[offset + 1u];
      offset += 2u;
    }
    if (length > size - offset) return H2_FM175XX_ERR_PROTOCOL;
    if (ntag_pages_protected(layout->protected_pages, start, offset + length))
      return H2_FM175XX_ERR_UNAVAILABLE;
    if (type == 3u) {
      layout->ndef_offset = start;
      layout->previous_end = offset + length;
      layout->previous_value_offset = offset;
      layout->previous_length = length;
      layout->previous_ndef_present = 1u;
      return H2_FM175XX_OK;
    }
    if (diagnostics != NULL)
      memcpy(diagnostics->control_tlv, raw + start,
             sizeof(diagnostics->control_tlv));
    int rc = ntag_control_tlv(reader, raw, size, type, offset, layout);
    if (rc != H2_FM175XX_OK) return rc;
    if (diagnostics != NULL) ++diagnostics->control_tlvs;
    offset += length;
    layout->ndef_offset = offset;
    layout->previous_end = offset;
  }
  return H2_FM175XX_OK;
}

static void ntag_message_sha256(const uint8_t* data, size_t length,
                                uint8_t digest[32]) {
  h2_sha256_t sha;
  h2_sha256_init(&sha);
  h2_sha256_update(&sha, data, length);
  h2_sha256_finish(&sha, digest);
}

static void ntag_init_write_diagnostics(
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  if (diagnostics != NULL) {
    memset(diagnostics, 0, sizeof(*diagnostics));
    diagnostics->last_command_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->last_restore_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->status_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->rx_status_result = H2_FM175XX_ERR_UNAVAILABLE;
    diagnostics->configuration_result = H2_FM175XX_ERR_UNAVAILABLE;
  }
}

int h2_fm175xx_ntag_prepare_ndef_update(
    h2_fm175xx_t* reader, h2_fm175xx_ndef_update_t* update,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  ntag_init_write_diagnostics(diagnostics);
  if (update == NULL) return H2_FM175XX_ERR_INVALID_ARG;
  update->size = 0u;
  if (reader == NULL) return H2_FM175XX_ERR_INVALID_ARG;
  uint8_t* raw = update->raw;
  size_t size = 0u;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_READ_TAG;
  int rc = ntag_read_all(reader, raw, sizeof(update->raw), &size,
                         diagnostics != NULL ? &diagnostics->preread : NULL);
  if (diagnostics != NULL && diagnostics->preread.required_capacity != 0u) {
    diagnostics->preread_header_valid = 1u;
    memcpy(diagnostics->preread_cc, raw + 12u, sizeof(diagnostics->preread_cc));
    memcpy(diagnostics->preread_static_locks, raw + 10u,
           sizeof(diagnostics->preread_static_locks));
  }
  if (rc != H2_FM175XX_OK) return rc;
  if (diagnostics != NULL) {
    diagnostics->tag_capacity = size;
    diagnostics->metadata_valid = 1u;
    memcpy(diagnostics->cc, raw + 12u, sizeof(diagnostics->cc));
    memcpy(diagnostics->static_locks, raw + 10u,
           sizeof(diagnostics->static_locks));
    if (size > 16u) diagnostics->first_tlv = raw[16];
    diagnostics->phase = H2_FM175XX_WRITE_CC;
  }
  if (size < 20u || raw[12] != 0xe1u || (raw[13] >> 4u) != 1u || raw[15] != 0u)
    return H2_FM175XX_ERR_UNAVAILABLE;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_LOCKS;
  if (raw[10] != 0u || raw[11] != 0u) return H2_FM175XX_ERR_UNAVAILABLE;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_LAYOUT;
  ntag_write_layout_t layout;
  rc = ntag_find_write_layout(reader, raw, size, &layout, diagnostics);
  if (rc != H2_FM175XX_OK) return rc;
  size_t start = layout.ndef_offset;
  if (diagnostics != NULL) diagnostics->ndef_offset = start;
  if (diagnostics != NULL && layout.previous_ndef_present) {
    diagnostics->before_ndef_length = layout.previous_length;
    ntag_message_sha256(raw + layout.previous_value_offset,
                        layout.previous_length,
                        diagnostics->before_ndef_sha256);
    diagnostics->before_ndef_valid = 1u;
  }
  /* Do not overwrite other data/control TLVs after an existing message. */
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_TRAILING_TLV;
  for (size_t i = layout.previous_end; i < size; ++i) {
    if (raw[i] == 0xfeu) break;
    if (raw[i] != 0u) return H2_FM175XX_ERR_UNAVAILABLE;
  }
  update->size = size;
  update->ndef_offset = start;
  update->value_offset = layout.previous_value_offset;
  update->value_length = layout.previous_length;
  memcpy(update->protected_pages, layout.protected_pages,
         sizeof(update->protected_pages));
  return H2_FM175XX_OK;
}

static int ntag_commit_ndef_update(h2_fm175xx_t* reader,
                                   h2_fm175xx_ndef_update_t* update,
                                   const uint8_t* data, size_t length,
                                   h2_fm175xx_write_diagnostics_t* diagnostics,
                                   bool stage_shared_page) {
  if (update == NULL) return H2_FM175XX_ERR_INVALID_ARG;
  size_t size = update->size;
  update->size = 0u;
  if (reader == NULL || data == NULL || length == 0u || length > 1003u ||
      size < 20u || size > sizeof(update->raw))
    return H2_FM175XX_ERR_INVALID_ARG;
  size_t start = update->ndef_offset;
  if (start < 16u || start >= size) return H2_FM175XX_ERR_INVALID_ARG;
  if (diagnostics != NULL) {
    ntag_message_sha256(data, length, diagnostics->expected_ndef_sha256);
    diagnostics->expected_ndef_valid = 1u;
    diagnostics->tag_capacity = size;
    diagnostics->ndef_offset = start;
  }
  uint8_t* raw = update->raw;
  size_t header = length < 255u ? 2u : 4u;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_CAPACITY;
  if (header + length + 1u > size - start) return H2_FM175XX_ERR_NO_MEMORY;
  size_t end = start + header + length + 1u;
  size_t page_end = (end + 3u) & ~(size_t)3u;
  if (page_end > size) return H2_FM175XX_ERR_NO_MEMORY;
  if (ntag_pages_protected(update->protected_pages, start, end))
    return H2_FM175XX_ERR_UNAVAILABLE;
  size_t length_offset = start + 1u;
  size_t length_page = length_offset & ~(size_t)3u;
  uint8_t empty[4];
  memcpy(empty, raw + length_page, sizeof(empty));
  empty[length_offset - length_page] = 0u;
  if (start >= length_page) empty[start - length_page] = 3u;
  if (!stage_shared_page && start + 2u < length_page + 4u)
    empty[start + 2u - length_page] = 0xfeu;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_INVALIDATE;
  int rc =
      write_ndef_page(reader, (uint8_t)(length_page / 4u), empty, diagnostics);
  if (rc != H2_FM175XX_OK) return rc;
  if (stage_shared_page && start < length_page && raw[start] != 3u) {
    uint8_t type_page[4];
    memcpy(type_page, raw + length_page - 4u, sizeof(type_page));
    type_page[3] = 3u;
    rc = write_ndef_page(reader, (uint8_t)(length_page / 4u - 1u), type_page,
                         diagnostics);
    if (rc != H2_FM175XX_OK) return rc;
  }
  memmove(raw + start + header, data, length);
  raw[start] = 3u;
  raw[length_offset] = 0u;
  if (header == 4u) {
    raw[start + 2u] = (uint8_t)(length >> 8u);
    raw[start + 3u] = (uint8_t)length;
  }
  raw[end - 1u] = 0xfeu;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_PAYLOAD;
  size_t first_byte = stage_shared_page ? start + 2u : start;
  for (size_t offset = first_byte & ~(size_t)3u;
       rc == H2_FM175XX_OK && offset < page_end; offset += 4u) {
    if (!stage_shared_page && offset == length_page) continue;
    rc = write_ndef_page(reader, (uint8_t)(offset / 4u), raw + offset,
                         diagnostics);
  }
  if (rc == H2_FM175XX_OK) {
    if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_PUBLISH;
    raw[length_offset] = length < 255u ? (uint8_t)length : 255u;
    rc = write_ndef_page(reader, (uint8_t)(length_page / 4u), raw + length_page,
                         diagnostics);
  }
  if (rc != H2_FM175XX_OK) return rc;
  if (diagnostics != NULL) diagnostics->phase = H2_FM175XX_WRITE_COMPLETE;
  return H2_FM175XX_OK;
}

int h2_fm175xx_ntag_commit_ndef_update(
    h2_fm175xx_t* reader, h2_fm175xx_ndef_update_t* update, const uint8_t* data,
    size_t length, h2_fm175xx_write_diagnostics_t* diagnostics) {
  ntag_init_write_diagnostics(diagnostics);
  return ntag_commit_ndef_update(reader, update, data, length, diagnostics,
                                 true);
}

int h2_fm175xx_ntag_write_ndef_diagnostic(
    h2_fm175xx_t* reader, const uint8_t* data, size_t length,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  if (reader == NULL || data == NULL || length == 0u || length > 1003u) {
    ntag_init_write_diagnostics(diagnostics);
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  h2_fm175xx_ndef_update_t update;
  int rc = h2_fm175xx_ntag_prepare_ndef_update(reader, &update, diagnostics);
  if (rc != H2_FM175XX_OK) return rc;
  return ntag_commit_ndef_update(reader, &update, data, length, diagnostics,
                                 false);
}
