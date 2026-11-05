#include "h2_esp_board.h"
#include "h2_esp_board_config.h"
#include "h2_esp_board_private.h"
#include "h2_esp_board_sleep_i2c.h"
#include "h2_esp_nfc_fm175xx.h"
#include "h2_esp_zero_esp_board_internal.h"
#include "h2_atomic.h"
#include "h2_atomic_static.h"

#define H2_ZERO_ESP_NFC_READ_CAP 256u

static h2_esp_nfc_fm175xx_t s_nfc;
static int s_nfc_initialized;
H2_ATOMIC_DEFINE_STATIC(bool, s_nfc_enabled, false);

/* Serialize scan, read and teardown without blocking either caller. */
H2_ATOMIC_DEFINE_STATIC(flag, s_nfc_busy, false);

void h2_esp_board_set_nfc_enabled(bool enabled) {
  h2_atomic_bool_store(&s_nfc_enabled, enabled, H2_ATOMIC_SEQ_CST);
}

bool h2_esp_board_nfc_enabled(void) {
  return h2_atomic_bool_load(&s_nfc_enabled, H2_ATOMIC_SEQ_CST);
}

static h2_pal_result_t ensure_nfc_adapter(void) {
  if (!h2_esp_board_nfc_enabled()) return H2_PAL_ERR_UNSUPPORTED;
  if (s_nfc_initialized != 0) {
    return H2_PAL_OK;
  }
  i2c_master_bus_handle_t bus = h2_esp_zero_esp_board_i2c_bus();
  if (bus == NULL) {
    return H2_PAL_ERR_UNAVAILABLE;
  }
  const h2_esp_nfc_fm175xx_config_t config = {
      .id = H2_ZERO_ESP_NFC_READER_ID,
      .bus = bus,
      .i2c_address = H2_ZERO_ESP_NFC_I2C_ADDR,
      .i2c_speed_hz = H2_ZERO_ESP_PERIPHERAL_I2C_SPEED_HZ,
      .read_capacity = H2_ZERO_ESP_NFC_READ_CAP,
  };
  h2_pal_result_t rc = h2_esp_nfc_fm175xx_init(&s_nfc, &config);
  if (rc != H2_PAL_OK) {
    return rc;
  }
  s_nfc_initialized = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t scan_nfc_reader(void* user, h2_pal_periph_id_t id,
                                       h2_pal_nfc_scan_t* out_scan) {
  (void)user;
  if (id != H2_ZERO_ESP_NFC_READER_ID) {
    h2_pal_periph_info_t info;
    if (h2_pal_periph_get(h2_esp_board_periph_api(), id, &info) == H2_PAL_OK) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (h2_atomic_flag_test_and_set(&s_nfc_busy, H2_ATOMIC_SEQ_CST))
    return H2_PAL_ERR_BUSY;
  h2_pal_result_t rc = H2_PAL_ERR_UNSUPPORTED;
  if (!h2_esp_board_nfc_enabled()) {
    if (s_nfc_initialized) {
      h2_esp_nfc_fm175xx_deinit(&s_nfc);
      s_nfc_initialized = 0;
      if (h2_zero_esp_sleep_nfc_prepare(h2_esp_zero_esp_board_i2c_bus()) !=
          ESP_OK)
        rc = H2_PAL_ERR_IO;
    }
  } else {
    rc = ensure_nfc_adapter();
    if (rc == H2_PAL_OK)
      rc = h2_pal_nfc_scan_nfc_reader(h2_esp_nfc_fm175xx_api(&s_nfc), id,
                                      out_scan);
  }
  h2_atomic_flag_clear(&s_nfc_busy, H2_ATOMIC_SEQ_CST);
  return rc;
}

static h2_pal_result_t read_nfc_data(void* user, h2_pal_periph_id_t id,
                                     const uint8_t* expected_uid,
                                     uint8_t expected_uid_len,
                                     h2_pal_nfc_data_type_t requested_type,
                                     const h2_pal_mem_api_t* allocator,
                                     h2_pal_nfc_data_read_t* out_data) {
  (void)user;
  if (id != H2_ZERO_ESP_NFC_READER_ID) {
    h2_pal_periph_info_t info;
    if (h2_pal_periph_get(h2_esp_board_periph_api(), id, &info) == H2_PAL_OK) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (!h2_esp_board_nfc_enabled()) return H2_PAL_ERR_UNSUPPORTED;
  if (h2_atomic_flag_test_and_set(&s_nfc_busy, H2_ATOMIC_SEQ_CST))
    return H2_PAL_ERR_BUSY;
  /* Only scan owns adapter initialization and power transitions. */
  h2_pal_result_t rc = H2_PAL_ERR_UNSUPPORTED;
  if (h2_esp_board_nfc_enabled() && s_nfc_initialized)
    rc = h2_pal_nfc_read_nfc_data(h2_esp_nfc_fm175xx_api(&s_nfc), id,
                                  expected_uid, expected_uid_len,
                                  requested_type, allocator, out_data);
  h2_atomic_flag_clear(&s_nfc_busy, H2_ATOMIC_SEQ_CST);
  return rc;
}

const h2_pal_nfc_api_t* h2_esp_board_nfc_api(void) {
  static const h2_pal_nfc_vtable_t vtable = {
      .scan_nfc_reader = scan_nfc_reader,
      .read_nfc_data = read_nfc_data,
  };
  static h2_pal_nfc_api_t api = {
      .vtable = &vtable,
  };
  return &api;
}
