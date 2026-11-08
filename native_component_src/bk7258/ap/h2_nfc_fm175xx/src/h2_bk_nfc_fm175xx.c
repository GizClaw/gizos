#include "h2_bk_nfc_fm175xx.h"

#include "h2_fm175xx_defs.h"

#include <common/bk_err.h>
#include <driver/i2c.h>
#include <os/os.h>
#include <stdio.h>
#include <string.h>

#define H2_BK_NFC_I2C_TIMEOUT_MS 20u
#define H2_BK_NFC_I2C_WRITE_MAX 32u
#define H2_BK_NFC_RF_TIMEOUT_MS 15u

static h2_pal_result_t pal_rc_from_fm(int rc) {
  switch (rc) {
    case H2_FM175XX_OK:
      return H2_PAL_OK;
    case H2_FM175XX_ERR_INVALID_ARG:
      return H2_PAL_ERR_INVALID_ARG;
    case H2_FM175XX_ERR_UNAVAILABLE:
      return H2_PAL_ERR_UNAVAILABLE;
    case H2_FM175XX_ERR_NO_MEMORY:
      return H2_PAL_ERR_NO_MEMORY;
    case H2_FM175XX_ERR_TIMEOUT:
      return H2_PAL_ERR_TIMEOUT;
    case H2_FM175XX_ERR_IO:
    case H2_FM175XX_ERR_PROTOCOL:
    default:
      return H2_PAL_ERR_IO;
  }
}

/* Trace control/status registers only: FIFO/card payload bytes stay private. */
static void trace_register(h2_bk_nfc_fm175xx_t* adapter, char operation,
                           uint8_t reg, uint8_t value) {
  if (reg != 0x01u && reg != 0x04u && reg != 0x06u && reg != 0x0au &&
      reg != 0x0cu && reg != 0x0du && reg != 0x12u && reg != 0x13u)
    return;
  const unsigned index = adapter->register_trace_count++ % 64u;
  adapter->register_trace[index] =
      ((uint32_t)(uint8_t)operation << 16u) | ((uint32_t)reg << 8u) | value;
}
static void report_register_trace(const h2_bk_nfc_fm175xx_t* adapter,
                                  int error) {
  char line[512];
  size_t used =
      (size_t)snprintf(line, sizeof(line), "H2_NFC_TRACE fm=%d", error);
  const unsigned count =
      adapter->register_trace_count > 64u ? 64u : adapter->register_trace_count;
  const unsigned start = adapter->register_trace_count - count;
  for (unsigned i = 0u; i < count && used + 9u < sizeof(line); ++i) {
    const uint32_t event = adapter->register_trace[(start + i) % 64u];
    const int written = snprintf(
        line + used, sizeof(line) - used, " %c%02x:%02x", (char)(event >> 16u),
        (unsigned)((event >> 8u) & 0xffu), (unsigned)(event & 0xffu));
    if (written <= 0) break;
    used += (size_t)written;
  }
  printf("%s\n", line);
}

static int stop_i2c_transport(h2_bk_nfc_fm175xx_t* adapter) {
  const int rc = bk_i2c_deinit(adapter->config.i2c_id);
  adapter->opened = 0;
  adapter->io_disabled = 1;
  adapter->io_poisoned = rc != BK_OK;
  return rc;
}

static int finish_i2c_transfer(h2_bk_nfc_fm175xx_t* adapter, int sdk_rc) {
  /* This SDK ignores a semaphore timeout and can report BK_OK while its ISR
   * still borrows the transfer buffer. Idle, not the SDK result alone, proves
   * that the borrow ended. A failed abort retains adapter-owned storage. */
  const int idle = bk_i2c_get_busstate((int)adapter->config.i2c_id) != 0u;
  if (sdk_rc == BK_OK && idle) return H2_FM175XX_OK;
  const int abort_rc = stop_i2c_transport(adapter);
  printf("H2_NFC_I2C_INCOMPLETE bus=%u sdk=%d idle=%d abort=%d\n",
         (unsigned)adapter->config.i2c_id, sdk_rc, idle, abort_rc);
  return H2_FM175XX_ERR_IO;
}

static int fm_i2c_write_regs(void* user, uint8_t reg, const uint8_t* data,
                             size_t len) {
  h2_bk_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || (data == NULL && len != 0u) ||
      len > H2_BK_NFC_I2C_WRITE_MAX) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  if (adapter->io_disabled || adapter->io_poisoned) return H2_FM175XX_ERR_IO;
  if (len == 0u) return H2_FM175XX_OK;
  uint8_t* buffer = adapter->io_tx_buffer;
  buffer[0] = reg;
  if (len != 0u) {
    memcpy(buffer + 1u, data, len);
  }
  const int sdk_rc =
      bk_i2c_master_write(adapter->config.i2c_id, adapter->config.i2c_address,
                          buffer, (uint32_t)len + 1u, H2_BK_NFC_I2C_TIMEOUT_MS);
  const int rc = finish_i2c_transfer(adapter, sdk_rc);
  if (rc == H2_FM175XX_OK && len == 1u)
    trace_register(adapter, 'W', reg, data[0]);
  if (rc != H2_FM175XX_OK)
    printf(
        "H2_NFC_I2C op=write bus=%u addr=0x%02x reg=0x%02x bytes=%u sdk=%d\n",
        (unsigned)adapter->config.i2c_id, (unsigned)adapter->config.i2c_address,
        (unsigned)reg, (unsigned)len, sdk_rc);
  return rc;
}

static int fm_i2c_write_reg(void* user, uint8_t reg, uint8_t value) {
  return fm_i2c_write_regs(user, reg, &value, 1u);
}

static int fm_i2c_read_regs(void* user, uint8_t reg, uint8_t* out_data,
                            size_t len) {
  h2_bk_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || (out_data == NULL && len != 0u) ||
      len > sizeof(adapter->io_rx_buffer)) {
    return H2_FM175XX_ERR_INVALID_ARG;
  }
  if (adapter->io_disabled || adapter->io_poisoned) return H2_FM175XX_ERR_IO;
  if (len == 0u) return H2_FM175XX_OK;
  memset(adapter->io_rx_buffer, 0, len);
  i2c_mem_param_t params = {
      .dev_addr = adapter->config.i2c_address,
      .mem_addr = reg,
      .mem_addr_size = I2C_MEM_ADDR_SIZE_8BIT,
      .data = adapter->io_rx_buffer,
      .data_size = (uint32_t)len,
      .timeout_ms = H2_BK_NFC_I2C_TIMEOUT_MS,
  };
  const int sdk_rc = bk_i2c_memory_read(adapter->config.i2c_id, &params);
  const int rc = finish_i2c_transfer(adapter, sdk_rc);
  if (rc == H2_FM175XX_OK) {
    memmove(out_data, adapter->io_rx_buffer, len);
    if (len == 1u) trace_register(adapter, 'R', reg, out_data[0]);
  }
  if (rc != H2_FM175XX_OK)
    printf("H2_NFC_I2C op=read bus=%u addr=0x%02x reg=0x%02x bytes=%u sdk=%d\n",
           (unsigned)adapter->config.i2c_id,
           (unsigned)adapter->config.i2c_address, (unsigned)reg, (unsigned)len,
           sdk_rc);
  return rc;
}

static int fm_i2c_read_reg(void* user, uint8_t reg, uint8_t* out_value) {
  return fm_i2c_read_regs(user, reg, out_value, 1u);
}

/* These reads do not consume FIFO data or expose a card's identity. */
static void report_activation(h2_bk_nfc_fm175xx_t* adapter, int result,
                              const h2_fm175xx_type_a_card_t* card) {
  const uint8_t addresses[] = {0x37u,
                               0x11u,
                               0x14u,
                               0x12u,
                               0x13u,
                               0x0du,
                               0x04u,
                               0x06u,
                               0x0au,
                               0x26u,
                               0x18u,
                               0x2au,
                               H2_FM175XX_REG_RX_SEL,
                               H2_FM175XX_REG_GSN,
                               H2_FM175XX_REG_GWGSP};
  uint8_t values[sizeof(addresses)] = {0};
  int reads = H2_FM175XX_OK;
  for (size_t i = 0u; i < sizeof(addresses); ++i) {
    const int rc = fm_i2c_read_reg(adapter, addresses[i], &values[i]);
    if (reads == H2_FM175XX_OK && rc != H2_FM175XX_OK) reads = rc;
  }
  printf(
      "H2_NFC_DIAG fm=%d atqa_seen=%u uid_bytes=%u wait_ms=%u fifo_cases=%u version=0x%02x "
      "mode=0x%02x txctrl=0x%02x txmode=0x%02x rxmode=0x%02x "
      "framing=0x%02x irq=0x%02x error=0x%02x fifo=%u gain=0x%02x "
      "threshold=0x%02x timer=0x%02x rxsel=0x%02x gsn=0x%02x gwgsp=0x%02x reads=%d\n",
      result, (unsigned)((card->atqa[0] | card->atqa[1]) != 0u),
      (unsigned)card->uid_len, (unsigned)H2_BK_NFC_RF_TIMEOUT_MS,
      adapter->fifo_verified_cases, (unsigned)values[0], (unsigned)values[1],
      (unsigned)values[2], (unsigned)values[3], (unsigned)values[4],
      (unsigned)values[5], (unsigned)values[6], (unsigned)values[7],
      (unsigned)values[8], (unsigned)values[9], (unsigned)values[10],
      (unsigned)values[11], (unsigned)values[12], (unsigned)values[13],
      (unsigned)values[14], reads);
}

static void fm_i2c_sleep_ms(void* user, uint32_t ms) {
  (void)user;
  rtos_delay_milliseconds(ms);
}

static int verify_fifo_transport(h2_bk_nfc_fm175xx_t* adapter) {
  /* Synthetic FIFO bytes are never transmitted to a tag. */
  static const uint8_t expected[] = {0x26u, 0x52u, 0x93u, 0x70u, 0x00u, 0xffu,
                                     0x55u, 0xaau, 0x01u, 0x80u, 0xfeu, 0x7fu,
                                     0x33u, 0xccu, 0x0fu, 0xf0u};
  static const uint8_t lengths[] = {1u, 2u, 5u, 7u, 16u};
  uint8_t actual[sizeof(expected)] = {0};
  int rc =
      fm_i2c_write_reg(adapter, H2_FM175XX_REG_COMMAND, H2_FM175XX_CMD_IDLE);
  unsigned completed = 0u;
  for (size_t i = 0u; rc == H2_FM175XX_OK && i < sizeof(lengths); ++i) {
    const size_t length = lengths[i];
    rc = fm_i2c_write_reg(adapter, H2_FM175XX_REG_FIFO_LEVEL, 0x80u);
    if (rc == H2_FM175XX_OK)
      rc = fm_i2c_write_regs(adapter, H2_FM175XX_REG_FIFO_DATA, expected,
                             length);
    if (rc == H2_FM175XX_OK)
      rc = fm_i2c_read_regs(adapter, H2_FM175XX_REG_FIFO_DATA, actual, length);
    if (rc == H2_FM175XX_OK && memcmp(actual, expected, length) != 0)
      rc = H2_FM175XX_ERR_IO;
    if (rc == H2_FM175XX_OK) ++completed;
  }
  const int cleanup =
      fm_i2c_write_reg(adapter, H2_FM175XX_REG_FIFO_LEVEL, 0x80u);
  if (rc == H2_FM175XX_OK) rc = cleanup;
  adapter->fifo_verified_cases = completed;
  printf("H2_NFC_FIFO_SELFTEST cases=%u result=%d\n", completed, rc);
  return rc;
}

static int disable_rf_field(h2_bk_nfc_fm175xx_t* adapter) {
  uint8_t control = 0u;
  int rc = fm_i2c_read_reg(adapter, H2_FM175XX_REG_TX_CTRL, &control);
  if (rc != H2_FM175XX_OK) return rc;
  rc = fm_i2c_write_reg(adapter, H2_FM175XX_REG_TX_CTRL,
                        (uint8_t)(control & (uint8_t)~0x03u));
  if (rc != H2_FM175XX_OK) return rc;
  rc = fm_i2c_read_reg(adapter, H2_FM175XX_REG_TX_CTRL, &control);
  if (rc == H2_FM175XX_OK && (control & 0x03u) != 0u) rc = H2_FM175XX_ERR_IO;
  return rc;
}

static h2_pal_result_t ensure_open(h2_bk_nfc_fm175xx_t* adapter) {
  if (adapter == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (adapter->io_poisoned) return H2_PAL_ERR_IO;
  if (adapter->opened != 0) {
    return H2_PAL_OK;
  }
  const i2c_config_t config = {
      .baud_rate = adapter->config.i2c_speed_hz,
      .addr_mode = I2C_ADDR_MODE_7BIT,
      .slave_addr = 0u,
  };
  const int driver_rc = bk_i2c_driver_init();
  const int init_rc = driver_rc == BK_OK
                          ? bk_i2c_init(adapter->config.i2c_id, &config)
                          : driver_rc;
  if (driver_rc != BK_OK || init_rc != BK_OK) {
    printf("H2_NFC_OPEN stage=i2c bus=%u driver=%d init=%d\n",
           (unsigned)adapter->config.i2c_id, driver_rc, init_rc);
    return H2_PAL_ERR_IO;
  }
  adapter->io_disabled = 0;
  uint8_t chip_version = 0u;
  const int version_rc = fm_i2c_read_reg(adapter, 0x37u, &chip_version);
  printf("H2_NFC_CHIP bus=%u addr=0x%02x version=0x%02x read=%d\n",
         (unsigned)adapter->config.i2c_id,
         (unsigned)adapter->config.i2c_address, (unsigned)chip_version,
         version_rc);
  if (version_rc != H2_FM175XX_OK) {
    (void)stop_i2c_transport(adapter);
    return pal_rc_from_fm(version_rc);
  }
  const h2_fm175xx_transport_t transport = {
      .user = adapter,
      .write_reg = fm_i2c_write_reg,
      .write_regs = fm_i2c_write_regs,
      .read_reg = fm_i2c_read_reg,
      .read_regs = fm_i2c_read_regs,
      .sleep_ms = fm_i2c_sleep_ms,
  };
  int rc = h2_fm175xx_init(&adapter->reader, &transport);
  if (rc == H2_FM175XX_OK) rc = verify_fifo_transport(adapter);
  if (rc == H2_FM175XX_OK) {
    if (adapter->rf_tuning) {
      const h2_fm175xx_type_a_profile_t profile = {
          .rf_config = adapter->rf_config,
          .rx_threshold = adapter->rx_threshold,
          .preserve_tx_drive = (uint8_t)adapter->preserve_tx_drive,
      };
      rc = h2_fm175xx_open_type_a_profile(&adapter->reader, &profile);
    } else {
      rc = h2_fm175xx_open_type_a(&adapter->reader);
    }
  }
  if (rc != H2_FM175XX_OK) {
    printf("H2_NFC_OPEN stage=type_a fm=%d\n", rc);
    (void)stop_i2c_transport(adapter);
    return pal_rc_from_fm(rc);
  }
  adapter->opened = 1;
  return H2_PAL_OK;
}

static uint8_t last_rf_error_bits(const h2_bk_nfc_fm175xx_t* adapter) {
  const unsigned count =
      adapter->register_trace_count > 64u ? 64u : adapter->register_trace_count;
  for (unsigned i = 0u; i < count; ++i) {
    const uint32_t event =
        adapter->register_trace[(adapter->register_trace_count - 1u - i) % 64u];
    if ((char)(event >> 16u) == 'R' &&
        ((event >> 8u) & 0xffu) == H2_FM175XX_REG_ERROR)
      return (uint8_t)event;
  }
  return 0u;
}

static int activate_card(h2_bk_nfc_fm175xx_t* adapter,
                         h2_fm175xx_type_a_card_t* card) {
  const unsigned limit = adapter->rf_tuning ? 3u : 1u;
  int protocol_failed = 0;
  int rc = H2_FM175XX_ERR_PROTOCOL;
  adapter->activation_attempts = 0u;
  adapter->rf_error_bits = 0u;
  adapter->register_trace_count = 0u;
  for (unsigned attempt = 0u; attempt < limit; ++attempt) {
    ++adapter->activation_attempts;
    if (adapter->rf_tuning) {
      const h2_fm175xx_type_a_profile_t profile = {
          .rf_config = adapter->rf_config,
          .rx_threshold = adapter->rx_threshold,
          .preserve_tx_drive = (uint8_t)adapter->preserve_tx_drive,
      };
      rc = h2_fm175xx_type_a_activate_profile(
          &adapter->reader, card, H2_BK_NFC_RF_TIMEOUT_MS, &profile);
    } else {
      rc = h2_fm175xx_type_a_activate_timed(&adapter->reader, card,
                                            H2_BK_NFC_RF_TIMEOUT_MS);
    }
    if (rc == H2_FM175XX_ERR_PROTOCOL) {
      protocol_failed = 1;
      adapter->rf_error_bits |= last_rf_error_bits(adapter);
    } else if (rc != H2_FM175XX_ERR_TIMEOUT || !protocol_failed) {
      break;
    }
    /* Only a later complete, validated selection can recover an RF error.
     * Silence after a malformed frame cannot manufacture an absent-card result.
     * Transport/register failures return immediately and are never retried. */
    if (attempt + 1u == limit) {
      rc = H2_FM175XX_ERR_PROTOCOL;
      break;
    }
    rc = disable_rf_field(adapter);
    if (rc != H2_FM175XX_OK) break;
    fm_i2c_sleep_ms(adapter, 5u);
  }
  if (protocol_failed)
    printf("H2_NFC_RF_RETRY attempts=%u fm=%d errors=0x%02x\n",
           adapter->activation_attempts, rc, (unsigned)adapter->rf_error_bits);
  return rc;
}

static h2_pal_result_t scan_nfc_reader(void* user, h2_pal_periph_id_t id,
                                       h2_pal_nfc_scan_t* out_scan) {
  h2_bk_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || out_scan == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != adapter->config.id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  memset(out_scan, 0, sizeof(*out_scan));
  out_scan->id = id;
  out_scan->stage = H2_PAL_NFC_STAGE_ERROR;
  h2_pal_result_t rc = ensure_open(adapter);
  if (rc != H2_PAL_OK) {
    out_scan->result = rc;
    return rc;
  }
  h2_fm175xx_type_a_card_t card;
  adapter->register_trace_count = 0u;
  ++adapter->scan_count;
  int fm_rc = activate_card(adapter, &card);
  if (fm_rc == H2_FM175XX_ERR_TIMEOUT) {
    if (adapter->last_scan_error != fm_rc || adapter->scan_count % 64u == 0u) {
      report_register_trace(adapter, fm_rc);
      report_activation(adapter, fm_rc, &card);
    }
    /* An unanswered request can leave a PICC in a partial selection state.
     * Drop this reader's field between failed polls; the next activation
     * restores Type A setup and powers the card up before polling again. */
    const int idle_rc = disable_rf_field(adapter);
    if (idle_rc != H2_FM175XX_OK) {
      out_scan->result = pal_rc_from_fm(idle_rc);
      return out_scan->result;
    }
    if (adapter->last_scan_error != fm_rc || adapter->scan_count % 64u == 0u)
      printf("H2_NFC_RF_RECOVER field=off result=0\n");
    adapter->last_scan_error = fm_rc;
    out_scan->stage = H2_PAL_NFC_STAGE_ABSENT;
    out_scan->result = H2_PAL_OK;
    return H2_PAL_OK;
  }
  if (fm_rc != H2_FM175XX_OK) {
    const int report =
        adapter->last_scan_error != fm_rc || adapter->scan_count % 16u == 0u;
    if (report) {
      printf("H2_NFC_SCAN stage=activate fm=%d pal=%d\n", fm_rc,
             pal_rc_from_fm(fm_rc));
      report_register_trace(adapter, fm_rc);
      report_activation(adapter, fm_rc, &card);
    }
    if (report && fm_rc == H2_FM175XX_ERR_PROTOCOL) {
      uint8_t irq = 0u, error = 0u, fifo = 0u, control = 0u;
      const int irq_rc = fm_i2c_read_reg(adapter, 0x04u, &irq);
      const int error_rc = fm_i2c_read_reg(adapter, 0x06u, &error);
      const int fifo_rc = fm_i2c_read_reg(adapter, 0x0au, &fifo);
      const int control_rc = fm_i2c_read_reg(adapter, 0x0cu, &control);
      printf(
          "H2_NFC_RF irq=0x%02x error=0x%02x fifo=%u control=0x%02x reads=%d,%d,%d,%d\n",
          (unsigned)irq, (unsigned)error, (unsigned)fifo, (unsigned)control,
          irq_rc, error_rc, fifo_rc, control_rc);
    }
    adapter->last_scan_error = fm_rc;
    out_scan->result = pal_rc_from_fm(fm_rc);
    return out_scan->result;
  }
  if (adapter->scan_count == 1u || adapter->last_scan_error != H2_FM175XX_OK ||
      adapter->scan_count % 16u == 0u)
    printf("H2_NFC_SELECTED scan=%u uid_bytes=%u attempts=%u\n",
           adapter->scan_count, (unsigned)card.uid_len,
           adapter->activation_attempts);
  adapter->last_scan_error = H2_FM175XX_OK;
  out_scan->stage = H2_PAL_NFC_STAGE_DISCOVERED;
  out_scan->result = H2_PAL_OK;
  out_scan->tag_type = H2_PAL_NFC_TAG_TYPE_ISO14443A;
  out_scan->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
                          ? H2_PAL_NFC_UID_MAX_LEN
                          : card.uid_len;
  memcpy(out_scan->uid, card.uid, out_scan->uid_len);
  return H2_PAL_OK;
}

static int uid_matches(const h2_fm175xx_type_a_card_t* card,
                       const uint8_t* expected_uid, uint8_t expected_uid_len) {
  return expected_uid_len == 0u ||
         (card->uid_len == expected_uid_len &&
          memcmp(card->uid, expected_uid, expected_uid_len) == 0);
}

static h2_pal_result_t read_nfc_data(void* user, h2_pal_periph_id_t id,
                                     const uint8_t* expected_uid,
                                     uint8_t expected_uid_len,
                                     h2_pal_nfc_data_type_t requested_type,
                                     const h2_pal_mem_api_t* allocator,
                                     h2_pal_nfc_data_read_t* out_data) {
  h2_bk_nfc_fm175xx_t* adapter = user;
  if (adapter == NULL || allocator == NULL || out_data == NULL ||
      (expected_uid == NULL && expected_uid_len != 0u) ||
      expected_uid_len > H2_PAL_NFC_UID_MAX_LEN) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_data, 0, sizeof(*out_data));
  if (id != adapter->config.id) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (requested_type != H2_PAL_NFC_DATA_NTAG_PAGES) {
    return H2_PAL_ERR_UNSUPPORTED;
  }
  h2_pal_result_t rc = ensure_open(adapter);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_fm175xx_type_a_card_t card;
  int fm_rc = activate_card(adapter, &card);
  if (fm_rc == H2_FM175XX_ERR_TIMEOUT) {
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (fm_rc != H2_FM175XX_OK) {
    return pal_rc_from_fm(fm_rc);
  }
  if (!uid_matches(&card, expected_uid, expected_uid_len)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  size_t capacity = adapter->config.read_capacity != 0u
                        ? adapter->config.read_capacity
                        : H2_BK_NFC_FM175XX_DEFAULT_READ_CAPACITY;
  uint8_t* bytes = h2_pal_mem_alloc(allocator, capacity);
  if (bytes == NULL) {
    return H2_PAL_ERR_NO_MEMORY;
  }
  size_t len = 0u;
  fm_rc = h2_fm175xx_ntag_read_all(&adapter->reader, bytes, capacity, &len);
  if (fm_rc != H2_FM175XX_OK) {
    h2_pal_mem_free(allocator, bytes);
    return pal_rc_from_fm(fm_rc);
  }
  out_data->id = id;
  out_data->tag_type = H2_PAL_NFC_TAG_TYPE_NTAG;
  out_data->uid_len = card.uid_len > H2_PAL_NFC_UID_MAX_LEN
                          ? H2_PAL_NFC_UID_MAX_LEN
                          : card.uid_len;
  memcpy(out_data->uid, card.uid, out_data->uid_len);
  out_data->type = H2_PAL_NFC_DATA_NTAG_PAGES;
  out_data->bytes = bytes;
  out_data->len = len;
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_nfc_fm175xx_init(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config) {
  static const h2_pal_nfc_vtable_t vtable = {
      .scan_nfc_reader = scan_nfc_reader,
      .read_nfc_data = read_nfc_data,
  };
  if (adapter == NULL || config == NULL || config->id == 0u ||
      config->i2c_address == 0u || config->i2c_speed_hz == 0u) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(adapter, 0, sizeof(*adapter));
  adapter->config = *config;
  adapter->api.user = adapter;
  adapter->api.vtable = &vtable;
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_nfc_fm175xx_init_with_rf(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config,
    uint8_t rf_config, uint8_t rx_threshold) {
  const h2_pal_result_t rc = h2_bk_nfc_fm175xx_init(adapter, config);
  if (rc != H2_PAL_OK) return rc;
  adapter->rf_config = rf_config;
  adapter->rx_threshold = rx_threshold;
  adapter->rf_tuning = 1;
  return H2_PAL_OK;
}

h2_pal_result_t h2_bk_nfc_fm175xx_init_with_profile(
    h2_bk_nfc_fm175xx_t* adapter, const h2_bk_nfc_fm175xx_config_t* config,
    const h2_fm175xx_type_a_profile_t* profile) {
  if (profile == NULL || profile->preserve_tx_drive > 1u)
    return H2_PAL_ERR_INVALID_ARG;
  const h2_pal_result_t rc = h2_bk_nfc_fm175xx_init_with_rf(
      adapter, config, profile->rf_config, profile->rx_threshold);
  if (rc == H2_PAL_OK) adapter->preserve_tx_drive = profile->preserve_tx_drive;
  return rc;
}

const h2_pal_nfc_api_t* h2_bk_nfc_fm175xx_api(h2_bk_nfc_fm175xx_t* adapter) {
  return adapter == NULL ? NULL : &adapter->api;
}
