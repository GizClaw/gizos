#include "h2_esp_nfc_fm175xx_internal.h"
#include "h2_app_test_mem.h"
#include "esp_log.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static struct {
  h2_fm175xx_type_a_card_t card;
  int activate_result, read_result, write_result;
  int activation_results[3];
  unsigned activation_result_count, activations, sleeps;
  unsigned reads, writes;
  char diagnostics[4096];
  size_t diagnostic_length;
} fixture;

static const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
static const uint8_t uid[] = {4u, 1u, 2u, 3u};

void h2_nfc_test_log(const char* tag, const char* format, ...) {
  assert(strcmp(tag, "h2_nfc_fm175xx") == 0);
  size_t available = sizeof(fixture.diagnostics) - fixture.diagnostic_length;
  va_list args;
  va_start(args, format);
  int length = vsnprintf(fixture.diagnostics + fixture.diagnostic_length,
                         available, format, args);
  va_end(args);
  assert(length >= 0 && (size_t)length + 1u < available);
  fixture.diagnostic_length += (size_t)length;
  fixture.diagnostics[fixture.diagnostic_length++] = '\n';
  fixture.diagnostics[fixture.diagnostic_length] = '\0';
}

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
      .last_reply_bits = 128u,
      .last_page = 4u,
      .status_result = H2_FM175XX_ERR_UNAVAILABLE,
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

int h2_fm175xx_ntag_write_ndef_diagnostic(
    h2_fm175xx_t* reader, const uint8_t* bytes, size_t length,
    h2_fm175xx_write_diagnostics_t* diagnostics) {
  int rc = h2_fm175xx_ntag_write_ndef(reader, bytes, length);
  *diagnostics = (h2_fm175xx_write_diagnostics_t){
      .phase = rc == H2_FM175XX_OK                ? H2_FM175XX_WRITE_COMPLETE
               : rc == H2_FM175XX_ERR_UNAVAILABLE ? H2_FM175XX_WRITE_CC
                                                  : H2_FM175XX_WRITE_VERIFY,
      .tag_capacity = 160u,
      .metadata_valid = 1u,
      .cc = {0xe1u, 0x10u, 18u, rc == H2_FM175XX_ERR_UNAVAILABLE ? 0x0fu : 0u},
      .first_tlv = 3u,
      .write_calls = rc == H2_FM175XX_ERR_UNAVAILABLE ? 0u : 1u,
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
  memcpy(fixture.card.uid, uid, sizeof(uid));
  fixture.card.uid_len = sizeof(uid);
  fixture.card.atqa[0] = 0x44u;
  fixture.card.sak_len = 1u;
  const h2_esp_nfc_fm175xx_config_t config = {
      .id = 201u,
      .bus = (i2c_master_bus_handle_t)&fixture,
      .i2c_address = 0x28u,
      .i2c_speed_hz = 400000u,
      .read_capacity = 128u};
  assert(h2_esp_nfc_fm175xx_init(adapter, &config) == H2_PAL_OK);
  adapter->opened = 1;
  adapter->reader.transport.user = &fixture;
  adapter->reader.transport.sleep_ms = selection_sleep;
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

int main(void) {
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
             "H2_NFC_WRITE phase=content fm=0 pal_rc=0 bytes=6 verified=1") !=
      NULL);
  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_PROTOCOL;
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_IO);
  assert(fixture.writes == 1u);
  assert(strstr(fixture.diagnostics, "verified=1") == NULL);
  setup(&adapter);
  fixture.write_result = H2_FM175XX_ERR_UNAVAILABLE;
  assert(write_ndef(&adapter) == H2_PAL_ERR_UNAVAILABLE);
  assert(fixture.activations == 1u && fixture.writes == 1u &&
         fixture.sleeps == 0u);
  assert(strstr(fixture.diagnostics, "H2_NFC_WRITE_DETAIL phase=cc fm=-2") !=
         NULL);
  assert(strstr(fixture.diagnostics, "write_calls=0") != NULL);
  assert(strstr(fixture.diagnostics, "verified=1") == NULL);
  setup(&adapter);
  fixture.card.uid[0] ^= 1u;
  assert(h2_pal_nfc_write_nfc_data(h2_esp_nfc_fm175xx_api(&adapter), 201u, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) ==
         H2_PAL_ERR_INVALID_STATE);
  assert(fixture.writes == 0u);
  return 0;
}
