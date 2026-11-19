#include "h2_esp_nfc_fm175xx_internal.h"
#include "h2_app_test_mem.h"
#include "h2_app_test_time.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static struct {
  h2_fm175xx_type_a_card_t card;
  int activate_result, read_result, write_result, log_result;
  int activation_results[3];
  unsigned activation_result_count, activations, sleeps;
  unsigned reads, writes;
  h2_app_test_time_t clock;
  bool preread_failure;
  char diagnostics[4096];
  size_t diagnostic_length;
} fixture;

static const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
static const uint8_t uid[] = {4u, 1u, 2u, 3u};

static int capture_log(void* user, h2_pal_log_level_t level, const char* scope,
                       const char* log_message) {
  assert(user == &fixture && level == H2_PAL_LOG_WARN);
  assert(strcmp(scope, "h2_nfc_fm175xx") == 0);
  size_t available = sizeof(fixture.diagnostics) - fixture.diagnostic_length;
  size_t length = strlen(log_message);
  assert(length + 1u < available);
  memcpy(fixture.diagnostics + fixture.diagnostic_length, log_message, length);
  fixture.diagnostic_length += length;
  fixture.diagnostics[fixture.diagnostic_length++] = '\n';
  fixture.diagnostics[fixture.diagnostic_length] = '\0';
  return fixture.log_result;
}

static const h2_pal_log_vtable_t log_vtable = {.write = capture_log};
static const h2_pal_log_api_t log_api = {.user = &fixture,
                                         .vtable = &log_vtable};

int h2_fm175xx_init(h2_fm175xx_t* reader,
                    const h2_fm175xx_transport_t* transport) {
  reader->transport = *transport;
  return H2_FM175XX_OK;
}

int h2_fm175xx_open_type_a(h2_fm175xx_t* reader) {
  (void)reader;
  return H2_FM175XX_OK;
}

int h2_fm175xx_type_a_activate(h2_fm175xx_t* reader,
                               h2_fm175xx_type_a_card_t* card) {
  (void)reader;
  *card = fixture.card;
  unsigned attempt = fixture.activations++;
  if (attempt < fixture.activation_result_count)
    return fixture.activation_results[attempt];
  return fixture.activate_result;
}

static void selection_sleep(void* user, uint32_t milliseconds) {
  assert(user == &fixture && milliseconds == 20u && fixture.writes == 0u);
  ++fixture.sleeps;
}

static int read_status(void* user, uint8_t address, uint8_t* value) {
  assert(user == &fixture && address < 64u);
  *value = 0u;
  return H2_FM175XX_OK;
}

int h2_fm175xx_type_a_activate_diagnostic(
    h2_fm175xx_t* reader, h2_fm175xx_type_a_card_t* card,
    h2_fm175xx_activation_diagnostics_t* diagnostics) {
  int rc = h2_fm175xx_type_a_activate(reader, card);
  *diagnostics = (h2_fm175xx_activation_diagnostics_t){
      .phase = rc == H2_FM175XX_OK ? H2_FM175XX_ACTIVATION_COMPLETE
                                   : H2_FM175XX_ACTIVATION_REQUEST,
      .command = 0x52u,
      .status_result = H2_FM175XX_ERR_UNAVAILABLE};
  return rc;
}

int h2_fm175xx_ntag_read_ndef(h2_fm175xx_t* reader, uint8_t* bytes,
                              size_t capacity, size_t* length) {
  (void)reader;
  ++fixture.reads;
  if (fixture.read_result != H2_FM175XX_OK) return fixture.read_result;
  assert(capacity >= sizeof(message));
  memcpy(bytes, message, sizeof(message));
  *length = sizeof(message);
  return H2_FM175XX_OK;
}

int h2_fm175xx_ntag_read_all(h2_fm175xx_t* reader, uint8_t* bytes,
                             size_t capacity, size_t* length) {
  return h2_fm175xx_ntag_read_ndef(reader, bytes, capacity, length);
}

int h2_fm175xx_ntag_read_ndef_diagnostic(
    h2_fm175xx_t* reader, uint8_t* bytes, size_t capacity, size_t* length,
    h2_fm175xx_read_diagnostics_t* diagnostics) {
  *diagnostics = (h2_fm175xx_read_diagnostics_t){
      .read_count = 2u,
      .last_reply_bits = fixture.read_result == H2_FM175XX_OK ? 128u : 96u,
      .last_page = 4u,
      .status_result = H2_FM175XX_ERR_UNAVAILABLE,
      .rx_status_result = H2_FM175XX_OK,
      .rx_irq = fixture.read_result == H2_FM175XX_OK ? 0x20u : 0x22u,
      .rx_error = fixture.read_result == H2_FM175XX_OK ? 0u : 4u,
      .rx_fifo = fixture.read_result == H2_FM175XX_OK ? 16u : 12u,
      .rx_control = 0x10u,
      .last_result = fixture.read_result};
  return h2_fm175xx_ntag_read_ndef(reader, bytes, capacity, length);
}

int h2_fm175xx_ntag_write_ndef(h2_fm175xx_t* reader, const uint8_t* bytes,
                               size_t length) {
  (void)reader;
  assert(length == sizeof(message) && memcmp(bytes, message, length) == 0);
  ++fixture.writes;
  return fixture.write_result;
}

int h2_fm175xx_ntag_prepare_ndef_update(
    h2_fm175xx_t* reader, h2_fm175xx_ndef_update_t* update,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  *diagnostics = (h2_fm175xx_write_diagnostics_t){0};
  update->size = sizeof(update->raw);
  update->value_offset = 18u;
  return h2_fm175xx_ntag_read_ndef_diagnostic(
      reader, update->raw + update->value_offset,
      sizeof(update->raw) - update->value_offset, &update->value_length,
      &diagnostics->preread);
}

int h2_fm175xx_ntag_commit_ndef_update(
    h2_fm175xx_t* reader, h2_fm175xx_ndef_update_t* update,
    const uint8_t* bytes, size_t length,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  assert(update->size != 0u);
  *diagnostics = (h2_fm175xx_write_diagnostics_t){0};
  update->size = 0u;
  return h2_fm175xx_ntag_write_ndef(reader, bytes, length);
}

int h2_fm175xx_ntag_write_ndef_diagnostic(
    h2_fm175xx_t* reader, const uint8_t* bytes, size_t length,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  int rc = h2_fm175xx_ntag_write_ndef(reader, bytes, length);
  if (fixture.preread_failure) {
    *diagnostics = (h2_fm175xx_write_diagnostics_t){
        .phase = H2_FM175XX_WRITE_READ_TAG,
        .preread = {.read_count = 21u,
                    .last_page = 80u,
                    .last_reply_bits = 4u,
                    .last_result = H2_FM175XX_ERR_PROTOCOL,
                    .transfer_result = H2_FM175XX_OK,
                    .required_capacity = 512u,
                    .short_reply_valid = 1u,
                    .short_reply_low4 = 0u,
                    .rx_status_result = H2_FM175XX_OK,
                    .rx_irq = 0x20u,
                    .rx_fifo = 1u,
                    .rx_control = 4u,
                    .status_result = H2_FM175XX_OK},
        .preread_header_valid = 1u,
        .preread_cc = {0xe1u, 0x10u, 62u, 0u}};
    return rc;
  }
  *diagnostics = (h2_fm175xx_write_diagnostics_t){
      .phase = rc == H2_FM175XX_OK                ? H2_FM175XX_WRITE_COMPLETE
               : rc == H2_FM175XX_ERR_UNAVAILABLE ? H2_FM175XX_WRITE_CC
                                                  : H2_FM175XX_WRITE_PUBLISH,
      .tag_capacity = 160u,
      .metadata_valid = 1u,
      .cc = {0xe1u, 0x10u, 18u, rc == H2_FM175XX_ERR_UNAVAILABLE ? 0x0fu : 0u},
      .first_tlv = 3u,
      .write_calls = rc == H2_FM175XX_ERR_UNAVAILABLE ? 0u : 1u,
      .last_reply_bits = 4u,
      .last_reply_low4 = rc == H2_FM175XX_OK ? 10u : 0u,
      .last_reply = {rc == H2_FM175XX_OK ? 10u : 0u, 0u, 0u},
      .rx_status_result = H2_FM175XX_OK,
      .configuration_result = H2_FM175XX_OK,
      .rx_irq = 0x64u,
      .rx_fifo = 1u,
      .rx_control = 0x14u,
      .last_tx_mode = 0x80u,
      .last_command_result = H2_FM175XX_OK,
      .last_restore_result = H2_FM175XX_OK,
      .status_result =
          rc == H2_FM175XX_OK ? H2_FM175XX_ERR_UNAVAILABLE : H2_FM175XX_OK,
      .last_page = rc == H2_FM175XX_ERR_UNAVAILABLE ? 0u : 4u};
  return rc;
}

int h2_esp_nfc_fm175xx_i2c_open(h2_esp_nfc_fm175xx_t* adapter,
                                h2_fm175xx_transport_t* transport) {
  (void)adapter;
  (void)transport;
  return H2_FM175XX_ERR_IO;
}

int h2_esp_nfc_fm175xx_spi_open(h2_esp_nfc_fm175xx_t* adapter,
                                h2_fm175xx_transport_t* transport) {
  return h2_esp_nfc_fm175xx_i2c_open(adapter, transport);
}

void h2_esp_nfc_fm175xx_i2c_close(h2_esp_nfc_fm175xx_t* adapter) {
  (void)adapter;
}

void h2_esp_nfc_fm175xx_spi_close(h2_esp_nfc_fm175xx_t* adapter) {
  (void)adapter;
}

static void setup(h2_esp_nfc_fm175xx_t* adapter) {
  memset(&fixture, 0, sizeof(fixture));
  h2_app_test_time_init(&fixture.clock, 0u);
  memcpy(fixture.card.uid, uid, sizeof(uid));
  fixture.card.uid_len = sizeof(uid);
  fixture.card.atqa[0] = 0x44u;
  fixture.card.sak_len = 1u;
  const h2_esp_nfc_fm175xx_config_t config = {
      .id = 201u,
      .bus = (i2c_master_bus_handle_t)&fixture,
      .i2c_address = 0x28u,
      .i2c_speed_hz = 400000u,
      .read_capacity = 128u,
      .time = &fixture.clock.api,
      .log = &log_api};
  assert(h2_esp_nfc_fm175xx_init(adapter, &config) == H2_PAL_OK);
  adapter->opened = 1;
  adapter->reader.transport.user = &fixture;
  adapter->reader.transport.sleep_ms = selection_sleep;
  adapter->reader.transport.read_reg = read_status;
}

static int read_ndef(h2_esp_nfc_fm175xx_t* adapter,
                     const h2_pal_mem_api_t* memory,
                     h2_pal_nfc_data_read_t* data) {
  return h2_pal_nfc_read_nfc_data(h2_esp_nfc_fm175xx_api(adapter), 201u, uid,
                                  sizeof(uid), H2_PAL_NFC_DATA_NDEF, memory,
                                  data);
}

static int write_ndef(h2_esp_nfc_fm175xx_t* adapter) {
  return h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message));
}

static void test_write_read_selection(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_esp_nfc_fm175xx_t adapter;
  h2_pal_nfc_data_read_t data;

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.activations == 1u && fixture.reads == 1u &&
         fixture.writes == 1u && adapter.read_selection_valid == 0);
  assert(strstr(fixture.diagnostics, "H2_NFC_WRITE_SELECT mode=read-session") !=
         NULL);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.activations == 2u && fixture.writes == 2u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  h2_pal_nfc_scan_t scan;
  assert(h2_pal_nfc_scan_nfc_reader(h2_esp_nfc_fm175xx_api(&adapter), 201u,
                                    &scan) == H2_PAL_OK);
  assert(adapter.read_selection_valid == 0);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.activations == 3u && fixture.writes == 1u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  fixture.read_result = H2_FM175XX_ERR_PROTOCOL;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_IO);
  assert(adapter.read_selection_valid == 0 && data.bytes == NULL);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.activations == 3u && fixture.writes == 1u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  uint8_t replacement_uid[sizeof(uid)];
  memcpy(replacement_uid, uid, sizeof(uid));
  replacement_uid[0] ^= 1u;
  memcpy(fixture.card.uid, replacement_uid, sizeof(replacement_uid));
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u,
                                   replacement_uid, sizeof(replacement_uid),
                                   H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_OK);
  assert(fixture.activations == 2u && fixture.writes == 1u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(h2_pal_nfc_write_nfc_data(
             h2_esp_nfc_fm175xx_api(&adapter), 201u, replacement_uid,
             sizeof(replacement_uid), H2_PAL_NFC_DATA_NDEF, message,
             sizeof(message)) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.activations == 2u && fixture.writes == 0u);

  setup(&adapter);
  fixture.card.sak[0] = 0x20u;
  assert(h2_pal_nfc_read_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                  sizeof(uid), H2_PAL_NFC_DATA_NTAG_PAGES,
                                  &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(write_ndef(&adapter) == H2_PAL_ERR_UNSUPPORTED);
  assert(fixture.activations == 1u && fixture.writes == 0u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(fixture.activations == 1u && fixture.writes == 1u &&
         adapter.read_selection_valid == 0);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  fixture.activate_result = H2_FM175XX_ERR_TIMEOUT;
  assert(h2_pal_nfc_scan_nfc_reader(h2_esp_nfc_fm175xx_api(&adapter), 201u,
                                    &scan) == H2_PAL_OK);
  assert(adapter.read_selection_valid == 0);
  assert(write_ndef(&adapter) == H2_PAL_ERR_NOT_FOUND);
  assert(fixture.activations == 5u && fixture.writes == 0u);

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  h2_pal_mem_free(&memory.api, data.bytes);
  h2_esp_nfc_fm175xx_deinit(&adapter);
  assert(adapter.read_selection_valid == 0 && adapter.opened == 0);
  assert(memory.live_blocks == 0u);
}

static void test_write_selection_recovery(void) {
  h2_esp_nfc_fm175xx_t adapter;
  setup(&adapter);
  fixture.activation_result_count = 2u;
  fixture.activation_results[0] = H2_FM175XX_ERR_PROTOCOL;
  fixture.activation_results[1] = H2_FM175XX_OK;
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.activations == 2u && fixture.sleeps == 1u &&
         fixture.writes == 1u);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_WRITE_SELECT attempt=1 limit=3 phase=request") != NULL);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_WRITE_SELECT attempt=2 limit=3 phase=complete") !=
         NULL);

  setup(&adapter);
  fixture.activation_result_count = 2u;
  fixture.activation_results[0] = H2_FM175XX_ERR_PROTOCOL;
  fixture.activation_results[1] = H2_FM175XX_ERR_TIMEOUT;
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(fixture.activations == 3u && fixture.sleeps == 2u &&
         fixture.writes == 1u);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_PROTOCOL;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(fixture.activations == 3u && fixture.sleeps == 2u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_TIMEOUT;
  assert(write_ndef(&adapter) == H2_PAL_ERR_NOT_FOUND);
  assert(fixture.activations == 3u && fixture.sleeps == 2u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_IO;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(fixture.activations == 3u && fixture.sleeps == 2u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_UNAVAILABLE;
  assert(write_ndef(&adapter) == H2_PAL_ERR_UNAVAILABLE);
  assert(fixture.activations == 1u && fixture.sleeps == 0u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.activation_result_count = 1u;
  fixture.activation_results[0] = H2_FM175XX_ERR_PROTOCOL;
  fixture.card.uid[0] ^= 1u;
  assert(write_ndef(&adapter) == H2_PAL_ERR_INVALID_STATE);
  assert(fixture.activations == 2u && fixture.sleeps == 1u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.card.sak[0] = 0x20u;
  assert(write_ndef(&adapter) == H2_PAL_ERR_UNSUPPORTED);
  assert(fixture.activations == 1u && fixture.sleeps == 0u &&
         fixture.writes == 0u);

  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(fixture.activations == 1u && fixture.sleeps == 0u &&
         fixture.writes == 1u);
}

static void test_prepared_ndef_write(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_esp_nfc_fm175xx_t adapter;
  h2_pal_nfc_data_read_t data = {0}, other = {0};
  h2_pal_nfc_scan_t scan;
  setup(&adapter);
  const h2_pal_nfc_api_t* api = h2_esp_nfc_fm175xx_api(&adapter);

  for (unsigned scenario = 0u; scenario < 5u; ++scenario) {
    unsigned reads = fixture.reads;
    unsigned activations = fixture.activations;
    unsigned writes = fixture.writes;
    assert(h2_pal_nfc_prepare_ndef_write(api, 201u, uid, sizeof(uid),
                                         &memory.api, &data) == H2_PAL_OK);
    assert(data.len == sizeof(message) &&
           memcmp(data.bytes, message, data.len) == 0);
    assert(h2_pal_nfc_scan_nfc_reader(api, 201u, &scan) == H2_PAL_ERR_BUSY);
    assert(read_ndef(&adapter, &memory.api, &other) == H2_PAL_ERR_BUSY);
    assert(write_ndef(&adapter) == H2_PAL_ERR_BUSY);
    assert(h2_pal_nfc_finish_ndef_write(api, 201u, &other, message,
                                        sizeof(message)) ==
           H2_PAL_ERR_INVALID_STATE);
    const uint8_t* reply = scenario == 1u ? NULL : message;
    size_t reply_length = reply != NULL ? sizeof(message) : 0u;
    fixture.write_result =
        scenario == 2u ? H2_FM175XX_ERR_TIMEOUT : H2_FM175XX_OK;
    if (scenario == 3u || scenario == 4u)
      assert(h2_app_test_time_advance(&fixture.clock, 500u) == H2_PAL_OK);
    if (scenario == 4u) {
      assert(h2_pal_nfc_scan_nfc_reader(api, 201u, &scan) == H2_PAL_OK);
      ++activations;
    }
    int expected = scenario >= 3u   ? H2_PAL_ERR_INVALID_STATE
                   : scenario == 2u ? H2_PAL_ERR_TIMEOUT
                                    : H2_PAL_OK;
    assert(h2_pal_nfc_finish_ndef_write(api, 201u, &data, reply,
                                        reply_length) == expected);
    assert(h2_pal_nfc_finish_ndef_write(api, 201u, &data, message,
                                        sizeof(message)) ==
           H2_PAL_ERR_INVALID_STATE);
    assert(fixture.activations == activations + 1u &&
           fixture.reads == reads + 1u &&
           fixture.writes == writes + (scenario == 0u || scenario == 2u));
    assert(h2_pal_nfc_scan_nfc_reader(api, 201u, &scan) == H2_PAL_OK);
    h2_pal_mem_free(&memory.api, data.bytes);
    fixture.diagnostic_length = 0u;
  }
  assert(memory.live_blocks == 0u);
}

static void test_optional_log(void) {
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_esp_nfc_fm175xx_t adapter;
  h2_pal_nfc_data_read_t data = {0};

  setup(&adapter);
  adapter.config.log = NULL;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.diagnostic_length == 0u && fixture.reads == 1u &&
         fixture.writes == 1u);
  h2_pal_mem_free(&memory.api, data.bytes);

  setup(&adapter);
  fixture.log_result = H2_PAL_ERR_IO;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  assert(write_ndef(&adapter) == H2_PAL_OK);
  assert(fixture.diagnostic_length != 0u && fixture.reads == 1u &&
         fixture.writes == 1u);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(memory.live_blocks == 0u);
}

int main(void) {
  test_optional_log();
  test_prepared_ndef_write();
  test_write_read_selection();
  test_write_selection_recovery();
  h2_app_test_mem_t memory;
  h2_app_test_mem_init(&memory, NULL);
  h2_esp_nfc_fm175xx_t adapter;
  h2_pal_nfc_data_read_t data;

  setup(&adapter);
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_OK);
  assert(data.type == H2_PAL_NFC_DATA_NDEF && data.len == sizeof(message) &&
         fixture.reads == 1u);
  assert(memcmp(data.bytes, message, sizeof(message)) == 0);
  assert(data.uid_len == sizeof(uid) &&
         memcmp(data.uid, uid, sizeof(uid)) == 0);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(memory.live_blocks == 0u);
  assert(strstr(fixture.diagnostics, "phase=activate fm=0 pal_rc=0") != NULL);
  assert(strstr(fixture.diagnostics, "phase=content fm=0 pal_rc=0") != NULL);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_PROTOCOL;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_IO);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(strstr(fixture.diagnostics, "phase=activate fm=-6 pal_rc=-4") != NULL);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_READ_SELECT phase=2 command=0x52 cascade=0 bits=0 "
                "fm=-6") != NULL);
  assert(strstr(fixture.diagnostics, "phase=content") == NULL);

  setup(&adapter);
  fixture.activate_result = H2_FM175XX_ERR_TIMEOUT;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_NOT_FOUND);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(strstr(fixture.diagnostics, "phase=activate fm=-5 pal_rc=-8") != NULL);

  setup(&adapter);
  fixture.card.uid[0] ^= 1u;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_INVALID_STATE);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(strstr(fixture.diagnostics, "phase=uid") != NULL);

  setup(&adapter);
  fixture.card.sak[0] = 0x20u;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_UNSUPPORTED);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(strstr(fixture.diagnostics, "phase=tag_type") != NULL);

  setup(&adapter);
  fixture.read_result = H2_FM175XX_ERR_PROTOCOL;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_IO);
  assert(data.bytes == NULL && fixture.reads == 1u);
  assert(memory.live_blocks == 0u);
  assert(strstr(fixture.diagnostics, "phase=content fm=-6 pal_rc=-4") != NULL);
  assert(strstr(fixture.diagnostics,
                "rx_status=0 rx_irq=0x22 rx_error=0x04 rx_fifo=12 "
                "rx_control=0x10") != NULL);

  setup(&adapter);
  memory.fail_at = memory.calls + 1u;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_NO_MEMORY);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(memory.live_blocks == 0u);
  assert(strstr(fixture.diagnostics, "phase=allocate") != NULL);
  memory.fail_at = 0u;

  setup(&adapter);
  adapter.opened = 0;
  assert(read_ndef(&adapter, &memory.api, &data) == H2_PAL_ERR_IO);
  assert(data.bytes == NULL && fixture.reads == 0u);
  assert(strstr(fixture.diagnostics, "phase=open") != NULL);

  setup(&adapter);
  assert(h2_pal_nfc_read_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                  sizeof(uid), H2_PAL_NFC_DATA_NTAG_PAGES,
                                  &memory.api, &data) == H2_PAL_OK);
  assert(data.type == H2_PAL_NFC_DATA_NTAG_PAGES);
  h2_pal_mem_free(&memory.api, data.bytes);
  assert(memory.live_blocks == 0u);
  setup(&adapter);
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_OK);
  assert(fixture.writes == 1u);
  assert(
      strstr(fixture.diagnostics,
             "H2_NFC_WRITE phase=content fm=0 pal_rc=0 bytes=6 submitted=1") !=
      NULL);
  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_IO);
  assert(fixture.writes == 1u);
  assert(strstr(fixture.diagnostics, "submitted=1") == NULL);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_WRITE_REPLY page=4 bits=4 low4=0x0 "
                "command_rc=0 restore_rc=0 status_rc=0") != NULL);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_WRITE_FRAME bytes=000000 rx_status=0 "
                "irq=0x64 error=0x00 fifo=1 control=0x14 "
                "config_status=0 tx=0x80 rx=0x00 framing=0x00") != NULL);
  assert(strstr(fixture.diagnostics, "H2_NFC_WRITE_DETAIL phase=publish") !=
         NULL);
  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  fixture.preread_failure = true;
  assert(write_ndef(&adapter) == H2_PAL_ERR_IO);
  assert(strstr(fixture.diagnostics,
                "H2_NFC_WRITE_PREREAD count=21 page=80 bits=4 fm=-6 "
                "transfer_rc=0 required=512 header_valid=1 cc=e1103e00") !=
         NULL);
  assert(strstr(fixture.diagnostics, "short_valid=1 low4=0x0") != NULL);
  assert(strstr(fixture.diagnostics, "write_calls=0") != NULL);
  assert(strstr(fixture.diagnostics, "H2_NFC_WRITE_REPLY") == NULL);
  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_UNAVAILABLE;
  assert(write_ndef(&adapter) == H2_PAL_ERR_UNAVAILABLE);
  assert(fixture.activations == 1u && fixture.writes == 1u &&
         fixture.sleeps == 0u);
  assert(strstr(fixture.diagnostics, "H2_NFC_WRITE_DETAIL phase=cc fm=-2") !=
         NULL);
  assert(strstr(fixture.diagnostics, "write_calls=0") != NULL);
  assert(strstr(fixture.diagnostics, "submitted=1") == NULL);
  setup(&adapter);
  fixture.card.uid[0] ^= 1u;
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.writes == 0u);
  return 0;
}
