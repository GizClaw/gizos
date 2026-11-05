#include "h2_esp_board.h"
#include "h2_esp_board_private.h"
#include "h2_esp_board_config.h"
#include "h2_esp_zero_esp_board_internal.h"
#include "h2_esp_nfc_fm175xx.h"
#include <assert.h>
static unsigned bus_calls, init_calls, deinit_calls, scans, reads, writes;
static h2_pal_result_t init_result = H2_PAL_OK;
static int bus_storage, device_storage;
i2c_master_bus_handle_t h2_esp_zero_esp_board_i2c_bus(void) {
  ++bus_calls;
  return (i2c_master_bus_handle_t)&bus_storage;
}
const h2_pal_periph_api_t* h2_esp_board_periph_api(void) { return NULL; }
h2_pal_result_t h2_esp_nfc_fm175xx_init(
    h2_esp_nfc_fm175xx_t* adapter, const h2_esp_nfc_fm175xx_config_t* config) {
  (void)adapter;
  assert(config->i2c_address == 0x28u);
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
const h2_pal_nfc_api_t* h2_esp_nfc_fm175xx_api(h2_esp_nfc_fm175xx_t* adapter) {
  (void)adapter;
  static const h2_pal_nfc_vtable_t vtable = {.scan_nfc_reader = scan,
                                             .read_nfc_data = read_card};
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
  *device = (i2c_master_dev_handle_t)&device_storage;
  return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
                              const uint8_t* data, size_t size,
                              int timeout_ms) {
  static const uint8_t expected[][2] = {{1, 0x20}, {0x14, 0x80}, {1, 0x30}};
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  assert(size == 2u && timeout_ms == 20);
  assert(data[0] == expected[writes % 3u][0]);
  assert(data[1] == expected[writes % 3u][1]);
  ++writes;
  return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device) {
  assert(device == (i2c_master_dev_handle_t)&device_storage);
  return ESP_OK;
}
int main(void) {
  const h2_pal_nfc_api_t* nfc = h2_esp_board_nfc_api();
  h2_pal_nfc_scan_t result = {0};
  h2_pal_nfc_data_read_t data = {0};
  const h2_pal_mem_api_t mem = {0};
  const h2_pal_periph_id_t id = H2_ZERO_ESP_NFC_READER_ID;
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
  assert(nfc->vtable->scan_nfc_reader(NULL, id, &result) == H2_PAL_OK);
  assert(init_calls == 1 && scans == 1);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_OK);
  assert(reads == 1);
  h2_esp_board_set_nfc_enabled(false);
  assert(writes == 0 && deinit_calls == 0);
  assert(nfc->vtable->read_nfc_data(NULL, id, NULL, 0, 0, &mem, &data) ==
         H2_PAL_ERR_UNSUPPORTED);
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
  return 0;
}
