#include "h2_esp_board_private.h"

#include "h2_esp_adc_oneshot.h"
#include "h2_esp_adc_stabilizer.h"
#include "h2_esp_board_config.h"
#include "h2_esp_h200_battery_profile.h"
#include "h2_esp_nfc_fm175xx.h"

#include "driver/gpio.h"

#include <string.h>

static h2_esp_adc_oneshot_t* s_adc;
static h2_esp_adc_percent_stabilizer_t s_percent_stabilizer;
static h2_esp_nfc_fm175xx_t s_nfc;
static int s_nfc_initialized;
static int s_buttons_initialized;
static int s_charge_initialized;

static int32_t battery_voltage_from_raw(int raw) {
  return h2_esp_h200_battery_voltage_from_raw(raw);
}

static const h2_pal_periph_led_strip_payload_t s_led_payload = {
    .data_gpio_pin_id = H2_H200_LED_STRIP_GPIO,
    .led_count = H2_H200_LED_STRIP_PIXEL_COUNT,
    .channels_per_led = H2_H200_LED_STRIP_CHANNELS_PER_PIXEL,
};

static const h2_pal_periph_pwm_switch_payload_t s_white_led_payload = {
    .pwm_channel_id = H2_H200_WHITE_LED_LEDC_CHANNEL,
    .gpio_pin_id = H2_H200_WHITE_LED_GPIO,
    .frequency_hz = H2_H200_WHITE_LED_FREQUENCY_HZ,
    .duty_resolution_bits = H2_H200_WHITE_LED_DUTY_RESOLUTION_BITS,
};

static int known_periph_id(h2_pal_periph_id_t id) {
  return id == H2_H200_BUTTON_POWER_ID || id == H2_H200_BUTTON_VOLUME_DEC_ID ||
         id == H2_H200_BUTTON_VOLUME_INC_ID || id == H2_H200_NFC_READER_ID ||
         id == H2_H200_BATTERY_ID || id == H2_H200_LED_STRIP_ID ||
         id == H2_H200_WHITE_LED_ID;
}

static h2_pal_result_t emit(h2_pal_periph_type_t filter, h2_pal_periph_cb_t cb,
                            void* cb_user, h2_pal_periph_info_t info) {
  return filter != H2_PAL_PERIPH_TYPE_ANY && filter != info.type
             ? H2_PAL_OK
             : cb(cb_user, &info);
}

static h2_pal_result_t periph_list(void* user, h2_pal_periph_type_t filter,
                                   h2_pal_periph_cb_t cb, void* cb_user) {
  (void)user;
  const h2_pal_periph_info_t infos[] = {
      {
          .id = H2_H200_BUTTON_POWER_ID,
          .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
          .name = "power",
      },
      {
          .id = H2_H200_BUTTON_VOLUME_DEC_ID,
          .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
          .name = "volume_decrement",
      },
      {
          .id = H2_H200_BUTTON_VOLUME_INC_ID,
          .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
          .name = "volume_increment",
      },
      {
          .id = H2_H200_NFC_READER_ID,
          .type = H2_PAL_PERIPH_TYPE_NFC_READER,
          .name = "nfc",
      },
      {
          .id = H2_H200_BATTERY_ID,
          .type = H2_PAL_PERIPH_TYPE_BATTERY,
          .name = "battery",
      },
      {
          .id = H2_H200_LED_STRIP_ID,
          .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
          .name = "led_strip",
          .payload = &s_led_payload,
          .payload_size = sizeof(s_led_payload),
      },
      {
          .id = H2_H200_WHITE_LED_ID,
          .type = H2_PAL_PERIPH_TYPE_PWM_SWITCH,
          .name = "white_led",
          .payload = &s_white_led_payload,
          .payload_size = sizeof(s_white_led_payload),
      },
  };
  for (size_t i = 0u; i < sizeof(infos) / sizeof(infos[0]); ++i) {
    h2_pal_result_t rc = emit(filter, cb, cb_user, infos[i]);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  return H2_PAL_OK;
}

typedef struct find_context {
  h2_pal_periph_id_t id;
  h2_pal_periph_info_t info;
} find_context_t;

static h2_pal_result_t find_cb(void* user, const h2_pal_periph_info_t* info) {
  find_context_t* context = (find_context_t*)user;
  if (context->id != info->id) {
    return H2_PAL_OK;
  }
  context->info = *info;
  return H2_PAL_ERR_WOULD_BLOCK;
}

static h2_pal_result_t periph_get(void* user, h2_pal_periph_id_t id,
                                  h2_pal_periph_info_t* out_info) {
  (void)user;
  find_context_t context = {.id = id};
  h2_pal_result_t rc =
      periph_list(NULL, H2_PAL_PERIPH_TYPE_ANY, find_cb, &context);
  if (rc != H2_PAL_ERR_WOULD_BLOCK) {
    return rc == H2_PAL_OK ? H2_PAL_ERR_NOT_FOUND : rc;
  }
  *out_info = context.info;
  return H2_PAL_OK;
}

static int button_gpio(h2_pal_periph_id_t id) {
  switch (id) {
    case H2_H200_BUTTON_POWER_ID:
      return H2_H200_BUTTON_POWER_GPIO;
    case H2_H200_BUTTON_VOLUME_DEC_ID:
      return H2_H200_BUTTON_VOLUME_DEC_GPIO;
    case H2_H200_BUTTON_VOLUME_INC_ID:
      return H2_H200_BUTTON_VOLUME_INC_GPIO;
    default:
      return -1;
  }
}

static h2_pal_result_t ensure_buttons(void) {
  if (s_buttons_initialized) {
    return H2_PAL_OK;
  }
  const gpio_config_t config = {
      .pin_bit_mask = (1ULL << H2_H200_BUTTON_POWER_GPIO) |
                      (1ULL << H2_H200_BUTTON_VOLUME_DEC_GPIO) |
                      (1ULL << H2_H200_BUTTON_VOLUME_INC_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&config) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_buttons_initialized = 1;
  return H2_PAL_OK;
}

static h2_pal_result_t read_single_button(
    void* user, h2_pal_periph_id_t id,
    h2_pal_single_button_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  const int gpio = button_gpio(id);
  if (gpio < 0) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = ensure_buttons();
  if (rc != H2_PAL_OK) {
    return rc;
  }
  *out_reading = (h2_pal_single_button_reading_t){
      .id = id,
      .state = gpio_get_level((gpio_num_t)gpio) == 0
                   ? H2_PAL_BUTTON_STATE_PRESSED
                   : H2_PAL_BUTTON_STATE_RELEASED,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t ensure_nfc(void) {
  if (s_nfc_initialized) {
    return H2_PAL_OK;
  }
  const h2_esp_nfc_fm175xx_spi_config_t config = {
      .id = H2_H200_NFC_READER_ID,
      .host = H2_H200_NFC_SPI_HOST,
      .cs_gpio = H2_H200_NFC_CS_GPIO,
      .sclk_gpio = H2_H200_NFC_SCLK_GPIO,
      .mosi_gpio = H2_H200_NFC_MOSI_GPIO,
      .miso_gpio = H2_H200_NFC_MISO_GPIO,
      .npd_gpio = H2_H200_NFC_NPD_GPIO,
      .clock_speed_hz = H2_H200_NFC_CLOCK_HZ,
      .read_capacity = H2_ESP_NFC_FM175XX_DEFAULT_READ_CAPACITY,
  };
  h2_pal_result_t rc = h2_esp_nfc_fm175xx_spi_init(&s_nfc, &config);
  if (rc == H2_PAL_OK) {
    s_nfc_initialized = 1;
  }
  return rc;
}

static h2_pal_result_t scan_nfc(void* user, h2_pal_periph_id_t id,
                                h2_pal_nfc_scan_t* out_scan) {
  (void)user;
  if (id != H2_H200_NFC_READER_ID) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = ensure_nfc();
  return rc == H2_PAL_OK ? h2_pal_nfc_scan_nfc_reader(
                               h2_esp_nfc_fm175xx_api(&s_nfc), id, out_scan)
                         : rc;
}

static h2_pal_result_t read_nfc(void* user, h2_pal_periph_id_t id,
                                const uint8_t* expected_uid,
                                uint8_t expected_uid_len,
                                h2_pal_nfc_data_type_t type,
                                const h2_pal_mem_api_t* allocator,
                                h2_pal_nfc_data_read_t* out_data) {
  (void)user;
  if (id != H2_H200_NFC_READER_ID) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = ensure_nfc();
  return rc == H2_PAL_OK ? h2_pal_nfc_read_nfc_data(
                               h2_esp_nfc_fm175xx_api(&s_nfc), id, expected_uid,
                               expected_uid_len, type, allocator, out_data)
                         : rc;
}

static h2_pal_result_t ensure_adc(void) {
  if (s_adc != NULL) {
    return H2_PAL_OK;
  }
  const h2_esp_adc_oneshot_channel_config_t channel = {
      .channel = H2_H200_BATTERY_ADC_CHANNEL,
      .oneshot =
          {
              .atten = ADC_ATTEN_DB_12,
              .bitwidth = ADC_BITWIDTH_DEFAULT,
          },
      .enable_calibration = false,
  };
  const h2_esp_adc_oneshot_config_t config = {
      .unit = {.unit_id = ADC_UNIT_1},
      .channels = &channel,
      .channel_count = 1u,
  };
  if (h2_esp_adc_oneshot_init(&config, &s_adc) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  if (h2_esp_adc_oneshot_stabilizer_init(s_adc, H2_H200_BATTERY_ADC_CHANNEL) !=
      ESP_OK) {
    (void)h2_esp_adc_oneshot_deinit(&s_adc);
    return H2_PAL_ERR_IO;
  }
  h2_esp_adc_percent_stabilizer_init(&s_percent_stabilizer);
  return H2_PAL_OK;
}

static h2_pal_result_t ensure_charge_gpio(void) {
  if (s_charge_initialized) {
    return H2_PAL_OK;
  }
  const gpio_config_t config = {
      .pin_bit_mask = (1ULL << H2_H200_CHARGE_INJECTION_GPIO) |
                      (1ULL << H2_H200_CHARGE_STATUS_GPIO),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  if (gpio_config(&config) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  s_charge_initialized = 1;
  return H2_PAL_OK;
}

static uint16_t battery_percent(const h2_esp_h200_battery_profile_t* profile,
                                int32_t voltage_mv) {
  if (voltage_mv <= profile->voltage_min_mv) {
    return 0u;
  }
  if (voltage_mv >= profile->voltage_max_mv) {
    return 10000u;
  }
  return (uint16_t)(((voltage_mv - profile->voltage_min_mv) * 10000) /
                    (profile->voltage_max_mv - profile->voltage_min_mv));
}

static h2_pal_result_t read_battery(void* user, h2_pal_periph_id_t id,
                                    h2_pal_battery_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  memset(out_reading, 0, sizeof(*out_reading));
  if (id != H2_H200_BATTERY_ID) {
    return known_periph_id(id) ? H2_PAL_ERR_INVALID_ARG : H2_PAL_ERR_NOT_FOUND;
  }
  h2_pal_result_t rc = ensure_adc();
  if (rc == H2_PAL_OK) {
    rc = ensure_charge_gpio();
  }
  if (rc != H2_PAL_OK) {
    return rc;
  }
  h2_esp_adc_value_reading_t voltage;
  if (h2_esp_adc_oneshot_read_value(s_adc, H2_H200_BATTERY_ADC_CHANNEL,
                                    &voltage) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  const h2_esp_h200_battery_profile_t* profile =
      h2_esp_h200_selected_battery_profile();
  const int32_t voltage_mv = battery_voltage_from_raw(voltage.stable_raw);
  const int injection =
      gpio_get_level((gpio_num_t)H2_H200_CHARGE_INJECTION_GPIO);
  const int charge_status =
      gpio_get_level((gpio_num_t)H2_H200_CHARGE_STATUS_GPIO);
  const bool charging = injection != 0;
  uint16_t percent_x100 = 0u;
  if (!h2_esp_adc_percent_stabilizer_update(
          &s_percent_stabilizer, battery_percent(profile, voltage_mv), charging,
          &percent_x100)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  uint32_t flags = H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_VOLTAGE_MV |
                   H2_PAL_BATTERY_HAS_PERCENT_X100;
  if (voltage_mv <= profile->shutdown_mv) {
    flags |= H2_PAL_BATTERY_LOW;
  }
  if (charging) {
    flags |= H2_PAL_BATTERY_CHARGING;
    if (charge_status != 0) {
      flags |= H2_PAL_BATTERY_FULL;
    }
  }
  *out_reading = (h2_pal_battery_reading_t){
      .id = id,
      .flags = flags,
      .voltage_mv = voltage_mv,
      .percent_x100 = percent_x100,
  };
  return H2_PAL_OK;
}

const h2_pal_periph_api_t* h2_esp_board_periph_api(void) {
  static const h2_pal_periph_vtable_t vtable = {
      .list = periph_list,
      .get = periph_get,
  };
  static const h2_pal_periph_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_button_api_t* h2_esp_board_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_single_button,
  };
  static const h2_pal_button_api_t api = {.vtable = &vtable};
  return &api;
}

const h2_pal_nfc_api_t* h2_esp_board_nfc_api(void) {
  static const h2_pal_nfc_vtable_t vtable = {
      .scan_nfc_reader = scan_nfc,
      .read_nfc_data = read_nfc,
  };
  static const h2_pal_nfc_api_t api = {.vtable = &vtable};
  return &api;
}

h2_pal_input_api_t* h2_esp_board_input_api(void) {
  static const h2_pal_input_vtable_t vtable = {
      .read_battery = read_battery,
  };
  static h2_pal_input_api_t api = {.vtable = &vtable};
  return &api;
}
