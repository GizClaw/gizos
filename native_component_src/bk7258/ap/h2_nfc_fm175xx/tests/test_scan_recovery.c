#include "h2_bk_nfc_fm175xx.h"
#include "h2_fm175xx_defs.h"
#include "common/bk_err.h"
#include "driver/i2c.h"

#include <assert.h>
#include <string.h>

static uint8_t registers[64];
static uint8_t fifo[64];
static size_t fifo_size;
static int corrupt_fifo;
static int reject_field_off;
static int fail_field_off;
static int deinit_calls;
static int activation_result;
static int activation_schedule[3];
static unsigned activation_schedule_count, activation_schedule_used;
static int defer_next_read;
static int pending_read;
static int fail_cancel;
static unsigned sdk_read_calls;
static i2c_mem_param_t pending_params;

bk_err_t bk_i2c_driver_init(void) { return BK_OK; }

bk_err_t bk_i2c_init(i2c_id_t id, const i2c_config_t* config) {
  assert(id == I2C_ID_1 && config->baud_rate == 100000u);
  assert(config->addr_mode == I2C_ADDR_MODE_7BIT);
  registers[0x37] = 0xa1u;
  return BK_OK;
}

bk_err_t bk_i2c_deinit(i2c_id_t id) {
  assert(id == I2C_ID_1);
  ++deinit_calls;
  if (fail_cancel) return BK_FAIL;
  pending_read = 0;
  return BK_OK;
}

uint8_t bk_i2c_get_busstate(int id) {
  assert(id == I2C_ID_1);
  return pending_read ? 0u : 1u;
}

bk_err_t bk_i2c_master_write(i2c_id_t id, uint32_t address, const uint8_t* data,
                             uint32_t size, uint32_t timeout_ms) {
  assert(id == I2C_ID_1 && address == 0x28u && timeout_ms == 20u);
  assert(size >= 2u && data[0] < sizeof(registers));
  uint8_t reg = data[0];
  if (reg == H2_FM175XX_REG_FIFO_DATA) {
    assert(fifo_size + size - 1u <= sizeof(fifo));
    memcpy(fifo + fifo_size, data + 1u, size - 1u);
    fifo_size += size - 1u;
    return BK_OK;
  }
  assert(size == 2u);
  if (reg == H2_FM175XX_REG_TX_CTRL && (data[1] & 3u) == 0u) {
    if (fail_field_off) return BK_FAIL;
    if (reject_field_off) return BK_OK;
  }
  if (reg == H2_FM175XX_REG_FIFO_LEVEL && (data[1] & 0x80u))
    fifo_size = 0u;
  else
    registers[reg] = data[1];
  return BK_OK;
}

static bk_err_t complete_read(i2c_id_t id, const i2c_mem_param_t* params) {
  assert(id == I2C_ID_1 && params->dev_addr == 0x28u);
  assert(params->mem_addr_size == I2C_MEM_ADDR_SIZE_8BIT);
  assert(params->timeout_ms == 20u && params->mem_addr < sizeof(registers));
  if (params->mem_addr == H2_FM175XX_REG_FIFO_DATA) {
    assert(params->data_size <= fifo_size);
    memcpy(params->data, fifo, params->data_size);
    if (corrupt_fifo && params->data_size) params->data[0] ^= 0x80u;
    memmove(fifo, fifo + params->data_size, fifo_size - params->data_size);
    fifo_size -= params->data_size;
  } else {
    assert(params->data_size == 1u);
    params->data[0] = params->mem_addr == H2_FM175XX_REG_FIFO_LEVEL
                          ? (uint8_t)fifo_size
                          : registers[params->mem_addr];
  }
  return BK_OK;
}

bk_err_t bk_i2c_memory_read(i2c_id_t id, const i2c_mem_param_t* params) {
  ++sdk_read_calls;
  if (defer_next_read) {
    defer_next_read = 0;
    pending_read = 1;
    pending_params = *params;
    /* Reproduce this SDK's successful return after an unreported timeout. */
    return BK_OK;
  }
  return complete_read(id, params);
}

static void complete_late_read(void) {
  assert(pending_read);
  assert(complete_read(I2C_ID_1, &pending_params) == BK_OK);
  pending_read = 0;
}

bk_err_t rtos_delay_milliseconds(uint32_t milliseconds) {
  (void)milliseconds;
  return BK_OK;
}

/* Isolate PAL/provider policy from the separately tested RF protocol. */
int h2_fm175xx_init(h2_fm175xx_t* reader,
                    const h2_fm175xx_transport_t* transport) {
  reader->transport = *transport;
  return H2_FM175XX_OK;
}

int h2_fm175xx_open_type_a(h2_fm175xx_t* reader) {
  return reader->transport.write_reg(reader->transport.user,
                                     H2_FM175XX_REG_TX_CTRL, 0x83u);
}

int h2_fm175xx_type_a_activate(h2_fm175xx_t* reader,
                               h2_fm175xx_type_a_card_t* card) {
  const int result = activation_schedule_used < activation_schedule_count
                         ? activation_schedule[activation_schedule_used++]
                         : activation_result;
  memset(card, 0, sizeof(*card));
  if ((registers[H2_FM175XX_REG_TX_CTRL] & 3u) != 3u)
    assert(h2_fm175xx_open_type_a(reader) == H2_FM175XX_OK);
  if (result == H2_FM175XX_OK) {
    card->uid_len = 4u;
    const uint8_t dummy_uid[] = {1u, 2u, 3u, 4u};
    memcpy(card->uid, dummy_uid, sizeof(dummy_uid));
  }
  return result;
}

int h2_fm175xx_type_a_activate_timed(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* card,
                                     uint32_t timeout_ms) {
  assert(timeout_ms == 15u);
  return h2_fm175xx_type_a_activate(reader, card);
}

int h2_fm175xx_type_a_activate_tuned(h2_fm175xx_t* reader,
                                     h2_fm175xx_type_a_card_t* card,
                                     uint32_t timeout_ms, uint8_t rf_config,
                                     uint8_t rx_threshold) {
  assert(rf_config == 0x7fu && rx_threshold == 0x84u);
  registers[H2_FM175XX_REG_RFCFG] = rf_config;
  registers[H2_FM175XX_REG_RX_THRES] = rx_threshold;
  return h2_fm175xx_type_a_activate_timed(reader, card, timeout_ms);
}

int h2_fm175xx_open_type_a_profile(h2_fm175xx_t* reader,
                                   const h2_fm175xx_type_a_profile_t* profile) {
  assert(profile && profile->rf_config == 0x7fu &&
         profile->rx_threshold == 0x84u && profile->preserve_tx_drive <= 1u);
  registers[H2_FM175XX_REG_GSN] = profile->preserve_tx_drive ? 0x98u : 0xf8u;
  registers[H2_FM175XX_REG_GWGSP] = profile->preserve_tx_drive ? 0x2au : 0x3fu;
  return h2_fm175xx_open_type_a(reader);
}

int h2_fm175xx_type_a_activate_profile(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* card, uint32_t timeout_ms,
    const h2_fm175xx_type_a_profile_t* profile) {
  assert(profile);
  if (profile->preserve_tx_drive)
    assert(registers[H2_FM175XX_REG_GSN] == 0x98u &&
           registers[H2_FM175XX_REG_GWGSP] == 0x2au);
  return h2_fm175xx_type_a_activate_tuned(
      reader, card, timeout_ms, profile->rf_config, profile->rx_threshold);
}

int h2_fm175xx_ntag_read_all(h2_fm175xx_t* reader, uint8_t* out,
                             size_t capacity, size_t* length) {
  (void)reader;
  (void)out;
  (void)capacity;
  (void)length;
  return H2_FM175XX_ERR_UNAVAILABLE;
}

static h2_pal_result_t scan(h2_bk_nfc_fm175xx_t* adapter,
                            h2_pal_nfc_scan_t* result) {
  const h2_pal_nfc_api_t* api = h2_bk_nfc_fm175xx_api(adapter);
  return api->vtable->scan_nfc_reader(api->user, 501u, result);
}

int main(void) {
  h2_bk_nfc_fm175xx_t adapter;
  const h2_bk_nfc_fm175xx_config_t config = {
      .id = 501u,
      .i2c_id = I2C_ID_1,
      .i2c_address = 0x28u,
      .i2c_speed_hz = 100000u,
  };
  h2_pal_nfc_scan_t result;
  assert(h2_bk_nfc_fm175xx_init(&adapter, &config) == H2_PAL_OK);
  activation_result = H2_FM175XX_ERR_TIMEOUT;
  assert(scan(&adapter, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_ABSENT && result.result == 0);
  assert(result.uid_len == 0u && fifo_size == 0u);
  assert(adapter.fifo_verified_cases == 5u);
  assert(registers[H2_FM175XX_REG_TX_CTRL] == 0x80u);
  /* The next poll can activate a real card; a successful selection keeps
   * RF available for the following content read. */
  activation_result = H2_FM175XX_OK;
  assert(scan(&adapter, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_DISCOVERED && result.uid_len == 4u);
  assert((registers[H2_FM175XX_REG_TX_CTRL] & 3u) == 3u);

  activation_result = H2_FM175XX_ERR_TIMEOUT;
  reject_field_off = 1;
  assert(scan(&adapter, &result) == H2_PAL_ERR_IO);
  assert(result.stage == H2_PAL_NFC_STAGE_ERROR &&
         result.result == H2_PAL_ERR_IO);
  reject_field_off = 0;
  fail_field_off = 1;
  assert(scan(&adapter, &result) == H2_PAL_ERR_IO);
  assert(result.stage == H2_PAL_NFC_STAGE_ERROR);
  fail_field_off = 0;
  assert(scan(&adapter, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_ABSENT);

  activation_result = H2_FM175XX_ERR_PROTOCOL;
  assert(scan(&adapter, &result) == H2_PAL_ERR_IO);
  assert(result.stage == H2_PAL_NFC_STAGE_ERROR);
  assert(adapter.activation_attempts == 1u);

  h2_bk_nfc_fm175xx_t tuned;
  assert(h2_bk_nfc_fm175xx_init_with_rf(&tuned, &config, 0x7fu, 0x84u) ==
         H2_PAL_OK);
  activation_schedule_count = 2u;
  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[1] = H2_FM175XX_OK;
  assert(scan(&tuned, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_DISCOVERED && result.uid_len == 4u);
  assert(tuned.activation_attempts == 2u && activation_schedule_used == 2u);
  assert(registers[H2_FM175XX_REG_RFCFG] == 0x7fu &&
         registers[H2_FM175XX_REG_RX_THRES] == 0x84u);

  /* Silence cannot erase a malformed-frame failure. A complete valid UID
   * selection, not merely a later timeout, is required to recover it. */
  activation_schedule_count = 3u;
  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[1] = H2_FM175XX_ERR_TIMEOUT;
  activation_schedule[2] = H2_FM175XX_ERR_TIMEOUT;
  assert(scan(&tuned, &result) == H2_PAL_ERR_IO);
  assert(result.stage == H2_PAL_NFC_STAGE_ERROR && result.uid_len == 0u);
  assert(tuned.activation_attempts == 3u && activation_schedule_used == 3u);
  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[1] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[2] = H2_FM175XX_ERR_PROTOCOL;
  assert(scan(&tuned, &result) == H2_PAL_ERR_IO);
  assert(tuned.activation_attempts == 3u && result.uid_len == 0u);

  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[1] = H2_FM175XX_ERR_IO;
  activation_schedule[2] = H2_FM175XX_OK;
  assert(scan(&tuned, &result) == H2_PAL_ERR_IO);
  assert(tuned.activation_attempts == 2u && activation_schedule_used == 2u);
  activation_schedule_count = 2u;
  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_IO;
  activation_schedule[1] = H2_FM175XX_OK;
  assert(scan(&tuned, &result) == H2_PAL_ERR_IO);
  assert(tuned.activation_attempts == 1u && activation_schedule_used == 1u);
  activation_schedule_used = 0u;
  activation_schedule[0] = H2_FM175XX_ERR_PROTOCOL;
  activation_schedule[1] = H2_FM175XX_OK;
  reject_field_off = 1;
  assert(scan(&tuned, &result) == H2_PAL_ERR_IO);
  assert(tuned.activation_attempts == 1u && result.uid_len == 0u);
  reject_field_off = 0;
  activation_schedule_count = activation_schedule_used = 0u;
  h2_bk_nfc_fm175xx_t preserving;
  h2_fm175xx_type_a_profile_t profile = {
      .rf_config = 0x7fu, .rx_threshold = 0x84u, .preserve_tx_drive = 1u};
  assert(h2_bk_nfc_fm175xx_init_with_profile(&preserving, &config, &profile) ==
         H2_PAL_OK);
  memset(&profile, 0,
         sizeof(profile)); /* The constructor copied every value. */
  activation_result = H2_FM175XX_OK;
  assert(scan(&preserving, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_DISCOVERED && result.uid_len == 4u);
  assert(registers[H2_FM175XX_REG_GSN] == 0x98u &&
         registers[H2_FM175XX_REG_GWGSP] == 0x2au);

  /* Register reads alone cannot prove that multi-byte FIFO transport works.
   * A corrupt self-check must reject startup and still clear its test bytes. */
  h2_bk_nfc_fm175xx_t broken;
  assert(h2_bk_nfc_fm175xx_init(&broken, &config) == H2_PAL_OK);
  corrupt_fifo = 1;
  int previous_deinit = deinit_calls;
  assert(scan(&broken, &result) == H2_PAL_ERR_IO);
  assert(result.stage == H2_PAL_NFC_STAGE_ERROR && broken.opened == 0);
  assert(fifo_size == 0u && deinit_calls == previous_deinit + 1);
  corrupt_fifo = 0;
  activation_result = H2_FM175XX_OK;
  assert(scan(&broken, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_DISCOVERED);

  h2_bk_nfc_fm175xx_t incomplete;
  assert(h2_bk_nfc_fm175xx_init(&incomplete, &config) == H2_PAL_OK);
  assert(scan(&incomplete, &result) == H2_PAL_OK);
  const h2_fm175xx_transport_t* transport = &incomplete.reader.transport;
  uint8_t caller_byte = 0x5au;
  previous_deinit = deinit_calls;
  defer_next_read = 1;
  assert(transport->read_reg(transport->user, H2_FM175XX_REG_TX_CTRL,
                             &caller_byte) == H2_FM175XX_ERR_IO);
  assert(caller_byte == 0x5au && pending_read == 0);
  assert(deinit_calls == previous_deinit + 1 && incomplete.opened == 0);
  /* A confirmed controller abort allows a clean reinitialization. */
  assert(scan(&incomplete, &result) == H2_PAL_OK);
  assert(result.stage == H2_PAL_NFC_STAGE_DISCOVERED);

  defer_next_read = 1;
  fail_cancel = 1;
  assert(transport->read_reg(transport->user, H2_FM175XX_REG_TX_CTRL,
                             &caller_byte) == H2_FM175XX_ERR_IO);
  assert(incomplete.io_poisoned && pending_read && caller_byte == 0x5au);
  const unsigned previous_reads = sdk_read_calls;
  assert(transport->read_reg(transport->user, 0x37u, &caller_byte) ==
         H2_FM175XX_ERR_IO);
  assert(scan(&incomplete, &result) == H2_PAL_ERR_IO);
  assert(sdk_read_calls == previous_reads);
  /* A delayed ISR completion after the failed call must not write to the
   * caller's output or stack. Only retained adapter storage may change. */
  complete_late_read();
  assert(pending_params.data[0] == 0x83u && caller_byte == 0x5au);
  fail_cancel = 0;
  assert(bk_i2c_deinit(I2C_ID_1) == BK_OK);
  return 0;
}
