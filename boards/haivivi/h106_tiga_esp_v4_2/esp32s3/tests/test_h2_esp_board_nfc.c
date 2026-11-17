#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_tiga_board_internal.h"
#include "h2_esp_nfc_fm175xx.h"
#include <assert.h>
static unsigned bus_calls, init_calls, deinit_calls, scans, reads, writes;
static unsigned nfc_writes;
static h2_pal_result_t init_result = H2_PAL_OK;
static h2_pal_result_t write_result = H2_PAL_OK;
static int bus_storage, device_storage;
static unsigned sequence_writes, fail_write;
i2c_master_bus_handle_t h2_esp_tiga_board_i2c_bus(void) {
  ++bus_calls;
  return (i2c_master_bus_handle_t)&bus_storage;
}
const h2_pal_periph_api_t* h2_esp_board_periph_api(void) { return NULL; }
h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t* adapter, const h2_esp_nfc_fm175xx_config_t* config) {
  (void)adapter;
  assert(config->i2c_address == 0x28u);
  assert(config->read_capacity == 512u);
  ++init_calls;
  return init_result;
}
static h2_pal_result_t scan(void* user, h2_pal_periph_id_t id,
                            h2_pal_nfc_scan_t* out) {
  (void)user;
  (void)id;
  (void)out;
  ++scans;
  return H2_PAL_OK;
}
static h2_pal_result_t read_card(void* user, h2_pal_periph_id_t id,
                                 const uint8_t* uid, uint8_t uid_len,
                                 h2_pal_nfc_data_type_t type,
                                 const h2_pal_mem_api_t* mem,
                                 h2_pal_nfc_data_read_t* out) {
  (void)user;
  (void)id;
  (void)uid;
  (void)uid_len;
  (void)type;
  (void)mem;
  (void)out;
  ++reads;
  return H2_PAL_OK;
}
static h2_pal_result_t write_card(void* user, h2_pal_periph_id_t id,
                                  const uint8_t* uid, uint8_t uid_len,
                                  h2_pal_nfc_data_type_t type,
                                  const uint8_t* bytes, size_t length) {
  (void)user;
  assert(id == H2_TIGA_ESP_NFC_READER_ID);
  assert(uid_len == 4u && uid[0] == 0x42u);
  assert(type == H2_PAL_NFC_DATA_NDEF);
  assert(length == 6u && bytes[0] == 0xd1u && bytes[5] == 'x');
  ++nfc_writes;
  h2_pal_nfc_scan_t scan_result;
  assert(h2_esp_board_nfc_api()->vtable->scan_nfc_reader(
             NULL, id, &scan_result) == H2_PAL_ERR_BUSY);
  return write_result;
}
const h2_pal_nfc_api_t* h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t* adapter) {
  (void)adapter;
  static const h2_pal_nfc_vtable_t vtable = {.scan_nfc_reader = scan,
                                             .read_nfc_data = read_card,
                                             .write_nfc_data = write_card};
  static const h2_pal_nfc_api_t api = {.vtable = &vtable};
  return &api;
}
void h2_esp_nfc_fm175xx_deinit(h2_esp_nfc_fm175xx_t* adapter) {
  (void)adapter;
  ++deinit_calls;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
                                    const i2c_device_config_t* config,
                                    i2c_master_dev_handle_t* device) {
  assert(bus == (i2c_master_bus_handle_t)&bus_storage);
  assert(config->device_address == 0x28u);
  sequence_writes = 0;
  *device = (i2c_master_dev_handle_t)&device_storage;
  return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t* data, size_t size,
                              int timeout_ms) {
  static const uint8_t expected[][2] = {{1, 0x20}, {0x14, 0x80}, {1, 0x30}};
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  assert(size == 2u && timeout_ms == 20);
  assert(data[0] == expected[sequence_writes % 3u][0]);
  assert(data[1] == expected[sequence_writes % 3u][1]);
  ++writes;
  ++sequence_writes;
  return sequence_writes == fail_write ? ESP_ERR_INVALID_STATE : ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  return ESP_OK;
}
int main(void) {
  const h2_pal_nfc_api_t* api = h2_esp_board_nfc_api();
  const uint8_t uid[] = {0x42u, 1u, 2u, 3u};
  const uint8_t message[] = {0xd1u, 1u, 2u, 'U', 4u, 'x'};
  assert(h2_pal_nfc_write_nfc_data(api, H2_TIGA_ESP_NFC_READER_ID, uid,
                                   sizeof(uid), H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_UNSUPPORTED);
  assert(writes == 0u && init_calls == 0u);
  const h2_pal_nfc_api_t* nfc = h2_esp_board_nfc_api();
  h2_pal_nfc_scan_t result = {0};
  h2_pal_nfc_data_read_t data = {0};
  const h2_pal_mem_api_t mem = {0};
  const h2_pal_periph_id_t id = H2_TIGA_ESP_NFC_READER_ID;
  assert(!h2_esp_board_nfc_enabled());
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(init_calls == 0 && bus_calls == 0);
  h2_esp_board_set_nfc_enabled(true); /* MFG's explicit startup path. */
  assert(init_calls == 0 && writes == 0);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(init_calls == 0);
  assert(h2_pal_nfc_write_nfc_data(api, id, uid, sizeof(uid),
                                   H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_UNSUPPORTED);
  assert(nfc_writes == 0u && init_calls == 0u);
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
  assert(init_calls == 1 && scans == 1);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_OK);
  assert(reads == 1);
  assert(h2_pal_nfc_write_nfc_data(api, id, uid, sizeof(uid),
                                   H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_OK);
  assert(nfc_writes == 1u);
  assert(h2_pal_nfc_write_nfc_data(api, id + 1u, uid, sizeof(uid),
                                   H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_NOT_FOUND);
  assert(nfc_writes == 1u);
  const h2_pal_result_t write_failures[] = {
      H2_PAL_ERR_IO, H2_PAL_ERR_TIMEOUT, H2_PAL_ERR_UNAVAILABLE,
      H2_PAL_ERR_UNSUPPORTED, H2_PAL_ERR_NO_MEMORY};
  for (size_t index = 0u;
       index < sizeof(write_failures) / sizeof(write_failures[0]); ++index) {
    write_result = write_failures[index];
    assert(h2_pal_nfc_write_nfc_data(api, id, uid, sizeof(uid),
                                     H2_PAL_NFC_DATA_NDEF, message,
                                     sizeof(message)) == write_result);
    assert(nfc_writes == index + 2u);
  }
  write_result = H2_PAL_OK;
  h2_esp_board_set_nfc_enabled(false);
  assert(writes == 0 && deinit_calls == 0);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(h2_pal_nfc_write_nfc_data(api, id, uid, sizeof(uid),
                                   H2_PAL_NFC_DATA_NDEF, message,
                                   sizeof(message)) == H2_PAL_ERR_UNSUPPORTED);
  assert(nfc_writes == 6u);
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(writes == 3 && deinit_calls == 1);
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) ==
         H2_PAL_ERR_UNSUPPORTED);
  assert(writes == 3);
  h2_esp_board_set_nfc_enabled(true);
  init_result = H2_PAL_ERR_IO;
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_ERR_IO);
  init_result = H2_PAL_OK;
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
  assert(init_calls == 3);
  for (unsigned failed = 1; failed <= 3; ++failed) {
    h2_esp_board_set_nfc_enabled(true);
    assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
    unsigned before_deinit = deinit_calls;
    h2_esp_board_set_nfc_enabled(false);
    fail_write = failed;
    assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_ERR_IO);
    assert(deinit_calls == before_deinit + 1u);
    unsigned before_writes = writes;
    fail_write = 0;
    assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(writes == before_writes + 3u && deinit_calls == before_deinit + 1u);
    before_writes = writes;
    assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) ==
           H2_PAL_ERR_UNSUPPORTED);
    assert(writes == before_writes);
  }
  /* Re-enabling cannot overwrite an incompletely powered-down reader. */
  h2_esp_board_set_nfc_enabled(true);
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
  unsigned before_init = init_calls;
  h2_esp_board_set_nfc_enabled(false);
  fail_write = 2;
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_ERR_IO);
  h2_esp_board_set_nfc_enabled(true);
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_ERR_IO);
  assert(init_calls == before_init);
  fail_write = 0;
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
  assert(init_calls == before_init + 1u);
  return 0;
}
