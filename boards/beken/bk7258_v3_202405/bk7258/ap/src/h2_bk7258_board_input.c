#include "h2_bk7258_board_private.h"

#if defined(H2_BK7258_BOARD_INPUT_HOST_TEST)
#include "h2_bk7258_board_input_test_sdk.h"
#else
#include <common/bk_err.h>
#include <driver/adc.h>
#include <driver/gpio.h>
#include <os/os.h>
#include "gpio_driver.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define H2_BK7258_SOC_TEMPERATURE_ID 501u
/* EVB V3 schematic sheet 2: J27 connects the K3..K6 ladder to P12/ADC14. */
#define H2_BK7258_ADC_KEY_GPIO GPIO_12
#define H2_BK7258_ADC_KEY_CHAN 14

extern bk_err_t bk_phy_get_current_temperature(float* temperature);

static bool s_adc_release_pending;
static unsigned s_adc_reported_errors;

static const h2_pal_periph_radio_button_payload_t s_adc_button_payload = {
    .group_id = H2_BK7258_ADC_BUTTON_GROUP_ID,
};

static h2_pal_result_t map_bk_rc(bk_err_t rc) {
  if (rc == BK_OK) {
    return H2_PAL_OK;
  }
  if (rc == BK_ERR_NULL_PARAM || rc == BK_ERR_PARAM) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_IO;
}

static h2_pal_result_t adc_result(const char* stage, bk_err_t native) {
  if (native != BK_OK && s_adc_reported_errors < 16u) {
    ++s_adc_reported_errors;
    printf("H2_BK_ADC_ERROR stage=%s native=%d\n", stage, (int)native);
  }
  return map_bk_rc(native);
}

static h2_pal_result_t adc_release(void) {
  h2_pal_result_t rc = adc_result("release", bk_adc_release());
  if (rc == H2_PAL_OK) {
    s_adc_release_pending = false;
  }
  return rc;
}

static h2_pal_result_t adc_read_raw(uint16_t* out_raw) {
  /* The SDK shares ADC power, configuration and ISR ownership with other users.
   * Restore the complete channel under its lock on every sampling transaction.
   */
  if (s_adc_release_pending) {
    h2_pal_result_t rc = adc_release();
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  h2_pal_result_t rc = adc_result("acquire", bk_adc_acquire());
  if (rc != H2_PAL_OK) {
    return rc;
  }
  s_adc_release_pending = true;

  bool initialized = false;
  bool started = false;
  uint16_t raw = 0, first_raw = 0;
  adc_config_t config;
  memset(&config, 0, sizeof(config));
  config.chan = (adc_chan_t)H2_BK7258_ADC_KEY_CHAN;
  config.adc_mode = ADC_CONTINUOUS_MODE;
  config.src_clk = ADC_SCLK_XTAL_26M;
  config.clk = 3203125;
  config.saturate_mode = ADC_SATURATE_MODE_3;
  config.steady_ctrl = 7;
  config.adc_filter = 0;

  rc = adc_result("gpio_unmap", gpio_dev_unmap(H2_BK7258_ADC_KEY_GPIO));
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  /* P12 is AP-owned. Keep the schematic's external R14 pull-up; selecting
   * ADC14 disables digital I/O, internal pulls and GPIO interrupts. */
  rc = adc_result("gpio_map",
                  gpio_dev_map(H2_BK7258_ADC_KEY_GPIO, GPIO_DEV_ADC14));
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  rc = adc_result("init", bk_adc_init(config.chan));
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  initialized = true;
  rc = adc_result("config", bk_adc_set_config(&config));
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  rc = adc_result("bypass", bk_adc_enable_bypass_clalibration());
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  rc = adc_result("start", bk_adc_start());
  if (rc != H2_PAL_OK) {
    goto cleanup;
  }
  started = true;
  rc = adc_result("channel", bk_adc_set_channel(config.chan));
  if (rc == H2_PAL_OK) {
    /* Discard the first conversion after restoring shared ADC power/config.
     * Keep continuous mode running while the analog input settles. */
    rc = adc_result("prime", bk_adc_read(&first_raw, 100));
    if (rc == H2_PAL_OK) rc = adc_result("settle", rtos_delay_milliseconds(5));
    if (rc == H2_PAL_OK) rc = adc_result("read", bk_adc_read(&raw, 100));
    static bool reported_config;
    if (rc == H2_PAL_OK && !reported_config) {
      printf("H2_BK_ADC_CONFIG gpio=%u channel=%u first=%u raw=%u\n",
             (unsigned)H2_BK7258_ADC_KEY_GPIO, (unsigned)H2_BK7258_ADC_KEY_CHAN,
             (unsigned)first_raw, (unsigned)raw);
      reported_config = true;
    }
  }

cleanup:
  if (started) {
    h2_pal_result_t stop = adc_result("stop", bk_adc_stop());
    if (rc == H2_PAL_OK) {
      rc = stop;
    }
  }
  if (initialized) {
    h2_pal_result_t deinit = adc_result("deinit", bk_adc_deinit(config.chan));
    if (rc == H2_PAL_OK) {
      rc = deinit;
    }
  }
  h2_pal_result_t release = adc_release();
  if (rc == H2_PAL_OK) {
    rc = release;
  }
  if (rc == H2_PAL_OK) {
    *out_raw = raw;
  }
  return rc;
}

static bool button_is_pressed(const h2_bk7258_button_adc_range_t* range,
                              uint16_t raw) {
  return raw >= range->min_raw && raw <= range->max_raw;
}

static h2_pal_periph_id_t pressed_radio_button_id(uint16_t raw) {
  for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
    const h2_bk7258_button_adc_range_t* range = &h2_bk7258_button_ranges[i];
    if (button_is_pressed(range, raw)) {
      return (h2_pal_periph_id_t)range->id;
    }
  }
  return 0u;
}

static int known_non_temperature_id(h2_pal_periph_id_t id) {
  if (id == H2_BK7258_ADC_BUTTON_GROUP_ID) {
    return 1;
  }
  for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
    if ((h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id == id) {
      return 1;
    }
  }
  return 0;
}

static const h2_bk7258_button_adc_range_t* find_button_range(
    h2_pal_periph_id_t id) {
  for (size_t i = 0; i < h2_bk7258_button_range_count; i += 1u) {
    if ((h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id == id) {
      return &h2_bk7258_button_ranges[i];
    }
  }
  return NULL;
}

static h2_pal_result_t periph_emit(h2_pal_periph_type_t type_filter,
                                   h2_pal_periph_cb_t cb, void* cb_user,
                                   h2_pal_periph_info_t info) {
  if (type_filter != H2_PAL_PERIPH_TYPE_ANY && info.type != type_filter) {
    return H2_PAL_OK;
  }
  return cb(cb_user, &info);
}

static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t type_filter,
                                   h2_pal_periph_cb_t cb, void* cb_user) {
  (void)user;

  h2_pal_result_t rc =
      periph_emit(type_filter, cb, cb_user,
                  (h2_pal_periph_info_t){
                      .id = H2_BK7258_ADC_BUTTON_GROUP_ID,
                      .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON_GROUP,
                      .name = "adc_keys",
                  });
  if (rc != H2_PAL_OK) {
    return rc;
  }

  for (size_t i = 0u; i < h2_bk7258_button_range_count; ++i) {
    rc =
        periph_emit(type_filter, cb, cb_user,
                    (h2_pal_periph_info_t){
                        .id = (h2_pal_periph_id_t)h2_bk7258_button_ranges[i].id,
                        .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
                        .name = "adc_button",
                        .payload = &s_adc_button_payload,
                        .payload_size = sizeof(s_adc_button_payload),
                    });
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }

  return periph_emit(type_filter, cb, cb_user,
                     (h2_pal_periph_info_t){
                         .id = H2_BK7258_SOC_TEMPERATURE_ID,
                         .type = H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
                         .name = "soc_temp",
                     });
}

typedef struct periph_find {
  h2_pal_periph_id_t id;
  h2_pal_periph_info_t info;
} periph_find_t;

static h2_pal_result_t periph_find_cb(void* user,
                                      const h2_pal_periph_info_t* info) {
  periph_find_t* find = (periph_find_t*)user;
  if (find == NULL || info == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (info->id != find->id) {
    return H2_PAL_OK;
  }
  find->info = *info;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out_info) {
  (void)user;
  periph_find_t find = {.id = id};
  h2_pal_result_t rc =
      periph_list(NULL, H2_PAL_PERIPH_TYPE_ANY, periph_find_cb, &find);
  if (rc != H2_PAL_ERR_WOULD_BLOCK) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_NOT_FOUND : rc;
  }
  *out_info = find.info;
  return H2_PAL_OK;
}

static h2_pal_result_t read_single_button(
    void* user, h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (find_button_range(id) != NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id == H2_BK7258_ADC_BUTTON_GROUP_ID ||
      id == H2_BK7258_SOC_TEMPERATURE_ID) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return H2_PAL_ERR_NOT_FOUND;
}

static h2_pal_result_t read_radio_button_group(
    void* user, h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_BK7258_ADC_BUTTON_GROUP_ID) {
    if (find_button_range(id) != NULL || id == H2_BK7258_SOC_TEMPERATURE_ID) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }

  uint16_t raw = 0;
  h2_pal_result_t rc = adc_read_raw(&raw);
  if (rc != H2_PAL_OK) {
    return rc;
  }

  h2_pal_periph_id_t pressed = pressed_radio_button_id(raw);
  static bool reported;
  static uint16_t last_raw;
  static h2_pal_periph_id_t last_pressed;
  uint32_t drift_raw = raw > last_raw ? raw - last_raw : last_raw - raw;
  /* Log changes without an image-lifetime count cap: interactive MFG retries
   * must remain observable after an earlier capture has consumed 64 events. */
  if (!reported || pressed != last_pressed || drift_raw >= 64u) {
    printf("H2_BK_ADC_SAMPLE raw=%u key=%lu\n", (unsigned)raw,
           (unsigned long)pressed);
    (void)fflush(stdout);
    reported = true;
    last_raw = raw;
    last_pressed = pressed;
  }
  *out_reading = (h2_pal_radio_button_group_reading_t){
      .id = id,
      .pressed_button_id = pressed,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_temperature(
    void* user, h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t* out_reading) {
  (void)user;
  if (id != H2_BK7258_SOC_TEMPERATURE_ID) {
    if (known_non_temperature_id(id)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }

  float celsius = 0.0f;
  if (bk_phy_get_current_temperature(&celsius) != BK_OK) {
    return H2_PAL_ERR_IO;
  }

  *out_reading = (h2_pal_temperature_reading_t){
      .id = id,
      .flags = H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS,
      .milli_celsius = (int32_t)(celsius * 1000.0f),
  };
  return H2_PAL_OK;
}

const h2_pal_button_api_t* h2_bk7258_board_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_single_button,
      .read_radio_button_group = read_radio_button_group,
  };
  static const h2_pal_button_api_t api = {
      .vtable = &vtable,
  };
  return &api;
}

h2_pal_input_api_t* h2_bk7258_board_input_api(void) {
  static const h2_pal_input_vtable_t vtable = {
      .read_temperature = read_temperature,
  };
  static h2_pal_input_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}

const h2_pal_periph_api_t* h2_bk7258_board_periph_api(void) {
  static const h2_pal_periph_vtable_t vtable = {
      .list = periph_list,
      .get = periph_get,
  };
  static const h2_pal_periph_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}
