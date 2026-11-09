#include "h2_esp_board_private.h"

#include "h2_esp_board.h"
#include "h2_esp_board_battery_curve.h"
#include "h2_esp_board_button_adc.h"
#include "h2_esp_board_config.h"
#include "h2_esp_zero_esp_board_internal.h"
#include "h2_esp_adc_oneshot.h"
#include "h2_esp_adc_stabilizer.h"

#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "esp_log.h"

#include <string.h>

#define H2_ZERO_ESP_ADC_CHANNEL_BTNS ADC_CHANNEL_2
#define H2_ZERO_ESP_ADC_CHANNEL_BATTERY ADC_CHANNEL_9
#define H2_ZERO_ESP_ADC_BUTTON_RELEASE_MIN_RAW 3800
#define H2_ZERO_ESP_ADC_BUTTON_RELEASE_MAX_RAW 4095
#define H2_ZERO_ESP_BATT_PRESENT_MIN_MV 2500

static temperature_sensor_handle_t s_temperature;
static h2_esp_adc_oneshot_t* s_adc;
static h2_esp_adc_radio_button_state_t s_adc_button_state;
static h2_esp_adc_percent_stabilizer_t s_battery_percent_state;
static h2_esp_adc_binary_stabilizer_t s_charge_detect_state;
static int s_boot_button_configured;
static int s_charge_detect_configured;
static uint32_t s_last_battery_flags = UINT32_MAX;

static const h2_pal_periph_led_strip_payload_t s_led_strip_payload = {
    .data_gpio_pin_id = H2_ZERO_ESP_LED_STRIP_GPIO,
    .led_count = H2_ZERO_ESP_LED_STRIP_PIXEL_COUNT,
    .channels_per_led = H2_ZERO_ESP_LED_STRIP_CHANNELS_PER_PIXEL,
};

static const h2_pal_periph_radio_button_payload_t s_adc_button_payload = {
    .group_id = H2_ZERO_ESP_ADC_BUTTON_GROUP_ID,
};

static const h2_pal_periph_gpio_irq_payload_t s_modem_ri_payload = {
    .gpio_pin_id = H2_ZERO_ESP_MODEM_RI_GPIO,
};

static const h2_pal_periph_pwm_switch_payload_t s_vibration_payload = {
    .pwm_channel_id = H2_ZERO_ESP_VIBRATION_LEDC_CHANNEL,
    .gpio_pin_id = H2_ZERO_ESP_VIBRATION_GPIO,
    .frequency_hz = H2_ZERO_ESP_VIBRATION_FREQUENCY_HZ,
    .duty_resolution_bits = H2_ZERO_ESP_VIBRATION_DUTY_RESOLUTION_BITS,
};

static int known_periph_id(h2_pal_periph_id_t id);

static h2_pal_result_t ensure_adc(void) {
  if (s_adc != NULL) {
    return H2_PAL_OK;
  }

  const h2_esp_adc_oneshot_channel_config_t channels[] = {
      {
          .channel = H2_ZERO_ESP_ADC_CHANNEL_BTNS,
          .oneshot =
              {
                  .atten = ADC_ATTEN_DB_12,
                  .bitwidth = ADC_BITWIDTH_DEFAULT,
              },
          .enable_calibration = true,
      },
      {
          .channel = H2_ZERO_ESP_ADC_CHANNEL_BATTERY,
          .oneshot =
              {
                  .atten = ADC_ATTEN_DB_12,
                  .bitwidth = ADC_BITWIDTH_DEFAULT,
              },
          .enable_calibration = true,
      },
  };
  const h2_esp_adc_oneshot_config_t config = {
      .unit =
          {
              .unit_id = ADC_UNIT_1,
          },
      .channels = channels,
      .channel_count = sizeof(channels) / sizeof(channels[0]),
  };
  if (h2_esp_adc_oneshot_init(&config, &s_adc) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }

  if (h2_esp_adc_oneshot_stabilizer_init(
          s_adc, H2_ZERO_ESP_ADC_CHANNEL_BATTERY) != ESP_OK) {
    (void)h2_esp_adc_oneshot_deinit(&s_adc);
    return H2_PAL_ERR_IO;
  }

  h2_esp_adc_radio_button_init(&s_adc_button_state);
  h2_esp_adc_percent_stabilizer_init(&s_battery_percent_state);
  h2_esp_adc_binary_stabilizer_init(&s_charge_detect_state);
  return H2_PAL_OK;
}

h2_pal_result_t h2_esp_board_button_adc_read(int* out_raw,
                                             int* out_millivolts) {
  if (out_raw != NULL) *out_raw = 0;
  if (out_millivolts != NULL) *out_millivolts = -1;
  if (out_raw == NULL || out_millivolts == NULL) return H2_PAL_ERR_INVALID_ARG;
  h2_pal_result_t rc = ensure_adc();
  if (rc != H2_PAL_OK) return rc;
  int raw = 0, mv = -1;
  if (h2_esp_adc_oneshot_read_raw(s_adc, H2_ZERO_ESP_ADC_CHANNEL_BTNS, &raw) !=
      ESP_OK)
    return H2_PAL_ERR_IO;
  const esp_err_t calibrated =
      h2_esp_adc_oneshot_read_mv(s_adc, H2_ZERO_ESP_ADC_CHANNEL_BTNS, &mv);
  if (calibrated != ESP_OK && calibrated != ESP_ERR_NOT_SUPPORTED)
    return H2_PAL_ERR_IO;
  *out_raw = raw;
  if (calibrated == ESP_OK) *out_millivolts = mv;
  return H2_PAL_OK;
}

static h2_esp_adc_radio_button_sample_kind_t adc_button_sample_kind(
    int raw, h2_pal_periph_id_t* out_button_id) {
  const h2_pal_periph_id_t button_id =
      h2_esp_zero_esp_adc_button_pressed_id(raw);
  if (button_id != 0u) {
    *out_button_id = button_id;
    return H2_ESP_ADC_RADIO_BUTTON_SAMPLE_PRESSED;
  }
  *out_button_id = 0u;
  if (raw >= H2_ZERO_ESP_ADC_BUTTON_RELEASE_MIN_RAW &&
      raw <= H2_ZERO_ESP_ADC_BUTTON_RELEASE_MAX_RAW) {
    return H2_ESP_ADC_RADIO_BUTTON_SAMPLE_RELEASED;
  }
  return H2_ESP_ADC_RADIO_BUTTON_SAMPLE_UNKNOWN;
}

static int known_periph_id(h2_pal_periph_id_t id) {
  return id == H2_ZERO_ESP_BOOT_BUTTON_ID ||
         id == H2_ZERO_ESP_ADC_BUTTON_GROUP_ID ||
         h2_esp_zero_esp_adc_button_id_known(id) ||
         id == H2_ZERO_ESP_NFC_READER_ID || id == H2_ZERO_ESP_MODEM_RI_ID ||
         id == H2_ZERO_ESP_BATTERY_ID || id == H2_ZERO_ESP_SOC_TEMPERATURE_ID ||
         id == H2_ZERO_ESP_LED_STRIP_ID || id == H2_ZERO_ESP_VIBRATION_ID;
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

  h2_pal_result_t rc = periph_emit(type_filter, cb, cb_user,
                                   (h2_pal_periph_info_t){
                                       .id = H2_ZERO_ESP_BOOT_BUTTON_ID,
                                       .type = H2_PAL_PERIPH_TYPE_SINGLE_BUTTON,
                                       .name = "boot",
                                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }

  rc = periph_emit(type_filter, cb, cb_user,
                   (h2_pal_periph_info_t){
                       .id = H2_ZERO_ESP_ADC_BUTTON_GROUP_ID,
                       .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON_GROUP,
                       .name = "adc_keys",
                   });
  if (rc != H2_PAL_OK) {
    return rc;
  }

  const h2_pal_periph_info_t button_infos[] = {
      {
          .id = H2_ZERO_ESP_BUTTON_OK_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "ok",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_LEFT_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "left",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_RIGHT_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "right",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_CANCEL_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "cancel",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_VOL_INC_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "vol_inc",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_VOL_DEC_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "vol_dec",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
      {
          .id = H2_ZERO_ESP_BUTTON_RECORD_ID,
          .type = H2_PAL_PERIPH_TYPE_RADIO_BUTTON,
          .name = "record",
          .payload = &s_adc_button_payload,
          .payload_size = sizeof(s_adc_button_payload),
      },
  };
  for (size_t i = 0u; i < sizeof(button_infos) / sizeof(button_infos[0]); ++i) {
    rc = periph_emit(type_filter, cb, cb_user, button_infos[i]);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }

  const h2_pal_periph_info_t other_infos[] = {
      {.id = H2_ZERO_ESP_NFC_READER_ID,
       .type = H2_PAL_PERIPH_TYPE_NFC_READER,
       .name = "nfc"},
      {
          .id = H2_ZERO_ESP_MODEM_RI_ID,
          .type = H2_PAL_PERIPH_TYPE_GPIO_IRQ,
          .name = "modem_ri",
          .payload = &s_modem_ri_payload,
          .payload_size = sizeof(s_modem_ri_payload),
      },
      {.id = H2_ZERO_ESP_BATTERY_ID,
       .type = H2_PAL_PERIPH_TYPE_BATTERY,
       .name = "battery"},
      {
          .id = H2_ZERO_ESP_LED_STRIP_ID,
          .type = H2_PAL_PERIPH_TYPE_LED_STRIP,
          .name = "led_strip",
          .payload = &s_led_strip_payload,
          .payload_size = sizeof(s_led_strip_payload),
      },
      {
          .id = H2_ZERO_ESP_SOC_TEMPERATURE_ID,
          .type = H2_PAL_PERIPH_TYPE_TEMPERATURE_SENSOR,
          .name = "soc_temp",
      },
      {
          .id = H2_ZERO_ESP_VIBRATION_ID,
          .type = H2_PAL_PERIPH_TYPE_PWM_SWITCH,
          .name = "vibration",
          .payload = &s_vibration_payload,
          .payload_size = sizeof(s_vibration_payload),
      },
  };
  for (size_t i = 0u; i < sizeof(other_infos) / sizeof(other_infos[0]); ++i) {
    rc = periph_emit(type_filter, cb, cb_user, other_infos[i]);
    if (rc != H2_PAL_OK) {
      return rc;
    }
  }
  return H2_PAL_OK;
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
  if (id != H2_ZERO_ESP_BOOT_BUTTON_ID) {
    if (known_periph_id(id)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (!s_boot_button_configured) {
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << (gpio_num_t)H2_ZERO_ESP_BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&config) != ESP_OK) {
      return H2_PAL_ERR_IO;
    }
    s_boot_button_configured = 1;
  }

  const int level = gpio_get_level((gpio_num_t)H2_ZERO_ESP_BOOT_BUTTON_GPIO);
  *out_reading = (h2_pal_single_button_reading_t){
      .id = id,
      .state = level == 0 ? H2_PAL_BUTTON_STATE_PRESSED
                          : H2_PAL_BUTTON_STATE_RELEASED,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_radio_button_group(
    void* user, h2_pal_periph_id_t id,
    h2_pal_radio_button_group_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_ZERO_ESP_ADC_BUTTON_GROUP_ID) {
    if (known_periph_id(id)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }

  h2_pal_result_t rc = ensure_adc();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  int adc_raw = 0;
  if (h2_esp_adc_oneshot_read_raw(s_adc, H2_ZERO_ESP_ADC_CHANNEL_BTNS,
                                  &adc_raw) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }

  h2_pal_periph_id_t decoded_button_id = 0u;
  const h2_esp_adc_radio_button_sample_kind_t sample_kind =
      adc_button_sample_kind(adc_raw, &decoded_button_id);
  uint32_t stable_button_id = 0u;
  if (!h2_esp_adc_radio_button_update(&s_adc_button_state, sample_kind,
                                      decoded_button_id, &stable_button_id)) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  *out_reading = (h2_pal_radio_button_group_reading_t){
      .id = id,
      .pressed_button_id = stable_button_id,
  };
  return H2_PAL_OK;
}

static h2_pal_result_t read_battery(void* user, h2_pal_periph_id_t id,
                                    h2_pal_battery_reading_t* out_reading) {
  (void)user;
  if (out_reading == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  if (id != H2_ZERO_ESP_BATTERY_ID) {
    if (known_periph_id(id)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }

  h2_pal_result_t rc = ensure_adc();
  if (rc != H2_PAL_OK) {
    return rc;
  }

  if (!s_charge_detect_configured) {
    const gpio_config_t charge_config = {
        .pin_bit_mask = 1ULL << (gpio_num_t)H2_ZERO_ESP_CHARGE_DETECT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&charge_config) != ESP_OK) {
      return H2_PAL_ERR_IO;
    }
    s_charge_detect_configured = 1;
  }

  h2_esp_adc_value_reading_t voltage;
  if (h2_esp_adc_oneshot_read_value(s_adc, H2_ZERO_ESP_ADC_CHANNEL_BATTERY,
                                    &voltage) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }
  if (voltage.reason == H2_ESP_ADC_VALUE_READ_JUMP) {
    h2_esp_adc_percent_stabilizer_init(&s_battery_percent_state);
  }

  const int charge_level =
      gpio_get_level((gpio_num_t)H2_ZERO_ESP_CHARGE_DETECT_GPIO);
  bool charging = false;
  if (!h2_esp_adc_binary_stabilizer_update(&s_charge_detect_state,
                                           charge_level != 0, &charging)) {
    return H2_PAL_ERR_INVALID_STATE;
  }
  const int32_t immediate_voltage_mv =
      h2_esp_zero_esp_battery_voltage_mv_from_raw(voltage.immediate_raw);
  const int32_t stable_voltage_mv =
      h2_esp_zero_esp_battery_voltage_mv_from_raw(voltage.stable_raw);
  uint16_t percent_x100 = 0u;
  if (!h2_esp_adc_percent_stabilizer_update(
          &s_battery_percent_state,
          h2_esp_zero_esp_battery_percent_x100(stable_voltage_mv, charging),
          charging, &percent_x100)) {
    return H2_PAL_ERR_INVALID_STATE;
  }

  const bool present = stable_voltage_mv >= H2_ZERO_ESP_BATT_PRESENT_MIN_MV;
  uint32_t flags = H2_PAL_BATTERY_HAS_VOLTAGE_MV;
  if (present) {
    flags |= H2_PAL_BATTERY_PRESENT | H2_PAL_BATTERY_HAS_PERCENT_X100;
  }
  if (present && immediate_voltage_mv <= H2_ESP_ZERO_BATTERY_USABLE_MIN_MV) {
    flags |= H2_PAL_BATTERY_LOW;
  }
  if (charging) {
    flags |= H2_PAL_BATTERY_CHARGING;
    if (present && percent_x100 >= 10000u) {
      flags |= H2_PAL_BATTERY_FULL;
    }
  }
  const h2_pal_battery_reading_t reading = {
      .id = id,
      .flags = flags,
      .voltage_mv = stable_voltage_mv,
      .percent_x100 = percent_x100,
  };

  *out_reading = reading;

  if (voltage.reason != H2_ESP_ADC_VALUE_READ_STEADY) {
    const char* read_reason =
        voltage.reason == H2_ESP_ADC_VALUE_READ_STARTUP
            ? "startup"
            : (voltage.reason == H2_ESP_ADC_VALUE_READ_JUMP ? "jump"
                                                            : "direct");
    ESP_LOGI("h2_zero_esp_input",
             "battery_read reason=%s immediate_mv=%ld stable_mv=%ld "
             "voltage_mv=%ld percent_x100=%u charging=%u flags=0x%08lx",
             read_reason, (long)immediate_voltage_mv, (long)stable_voltage_mv,
             (long)stable_voltage_mv, (unsigned int)percent_x100,
             charging ? 1u : 0u, (unsigned long)flags);
    s_last_battery_flags = flags;
  } else if (flags != s_last_battery_flags) {
    ESP_LOGI("h2_zero_esp_input",
             "battery charge_gpio=%u level=%d voltage_mv=%d percent_x100=%u "
             "flags=0x%08lx",
             H2_ZERO_ESP_CHARGE_DETECT_GPIO, charge_level,
             (int)stable_voltage_mv, (unsigned int)percent_x100,
             (unsigned long)flags);
    s_last_battery_flags = flags;
  }
  return H2_PAL_OK;
}

static h2_pal_result_t read_temperature(
    void* user, h2_pal_periph_id_t id,
    h2_pal_temperature_reading_t* out_reading) {
  (void)user;
  if (id != H2_ZERO_ESP_SOC_TEMPERATURE_ID) {
    if (known_periph_id(id)) {
      return H2_PAL_ERR_INVALID_ARG;
    }
    return H2_PAL_ERR_NOT_FOUND;
  }
  if (s_temperature == NULL) {
    const temperature_sensor_config_t config =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&config, &s_temperature) != ESP_OK) {
      return H2_PAL_ERR_UNAVAILABLE;
    }
    if (temperature_sensor_enable(s_temperature) != ESP_OK) {
      (void)temperature_sensor_uninstall(s_temperature);
      s_temperature = NULL;
      return H2_PAL_ERR_IO;
    }
  }

  float celsius = 0.0f;
  if (temperature_sensor_get_celsius(s_temperature, &celsius) != ESP_OK) {
    return H2_PAL_ERR_IO;
  }

  *out_reading = (h2_pal_temperature_reading_t){
      .id = id,
      .flags = H2_PAL_TEMPERATURE_HAS_MILLI_CELSIUS,
      .milli_celsius = (int32_t)(celsius * 1000.0f),
  };
  return H2_PAL_OK;
}

const h2_pal_button_api_t* h2_esp_board_button_api(void) {
  static const h2_pal_button_vtable_t vtable = {
      .read_single_button = read_single_button,
      .read_radio_button_group = read_radio_button_group,
  };
  static const h2_pal_button_api_t api = {
      .vtable = &vtable,
  };
  return &api;
}

h2_pal_input_api_t* h2_esp_board_input_api(void) {
  static const h2_pal_input_vtable_t vtable = {
      .read_battery = read_battery,
      .read_temperature = read_temperature,
  };
  static h2_pal_input_api_t api = {
      .user = NULL,
      .vtable = &vtable,
  };
  return &api;
}

const h2_pal_periph_api_t* h2_esp_board_periph_api(void) {
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
